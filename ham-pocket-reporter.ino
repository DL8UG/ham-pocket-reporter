/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <mail@dl8ug.de> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return. 73, DL8UG - Uwe
 * ----------------------------------------------------------------------------
 */

#include "config.h"               // TRACK_CALL, TEXT_SCALE, wifiList[], HISTORY_LINES
#include <GxEPD2_BW.h>            // E-paper display driver
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>          // NVS for caching last used WiFi

#include "dxcc_country_table.h"   // DXCC prefix → ISO‑3 country map


// ============================================================================
// ========================= DISPLAY CONFIGURATION ============================
// ============================================================================
//
// 2.9" Waveshare B/W V2 (GxEPD2_290_T94_V2) in partial refresh mode.
//

#define USE_HSPI_FOR_EPD           // Use HSPI bus instead of default VSPI
#define ENABLE_GxEPD2_GFX 0        // Disable GxEPD2_GFX (saves RAM & flash)

#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS  GxEPD2_290_T94_V2

#if defined(ESP32)
  #define MAX_DISPLAY_BUFFER_SIZE 65536ul
  #define MAX_HEIGHT(EPD) \
      (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) \
      ? EPD::HEIGHT \
      : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))
#endif

// Pin map (Waveshare ESP32 e‑Paper Driver Board):
// BUSY -> 25, RST -> 26, DC -> 27, CS -> 15, CLK -> 13, DIN -> 14
GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)>
  display(GxEPD2_DRIVER_CLASS(15, 27, 26, 25)); // (CS, DC, RST, BUSY)

#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
SPIClass hspi(HSPI); // HSPI instance (SCK, MISO, MOSI, SS are set below)
#endif


// ============================================================================
// ============================== WIFI & MQTT =================================
// ============================================================================

const char* mqtt_server = "mqtt.pskreporter.info";
const int   mqtt_port   = 1883;

WiFiClient      espClient;
PubSubClient    client(espClient);

// Will become: "pskr/filter/v2/+/+/<CALLSIGN>/#"
String topic_call;

// NVS cache for "last used SSID"
Preferences prefs;
static const char* PREF_NS  = "wifi";
static const char* PREF_KEY = "last_ssid";


// ============================================================================
// ========================= DISPLAY RUNTIME STATE =============================
// ============================================================================

// Active number of history lines (computed at runtime: fixed or auto-fit)
static uint8_t linesHistory = 6;  // set in setup()

// Fixed-size buffer; we only use [0..linesHistory-1]
static String lines[MAX_LINES_HISTORY];

static String bestDistLine = "";      // Best distance formatted line
static String bestSnrLine  = "";      // Best SNR formatted line

static float  bestDistVal  = -1;      // Best distance value in km
static int    bestSnrVal   = -999;    // Best SNR value in dB

String headerLine = "UTC   BAND  MODE  SNR   LOC  DIST  ISO  RX";


// ============================================================================
// ======================== TEXT GEOMETRY HELPERS ==============================
// ============================================================================

inline uint8_t  charW() { return 6 * TEXT_SCALE; }
inline uint8_t  charH() { return 8 * TEXT_SCALE; }
inline uint16_t LINE_STEP() { return charH() + 2; }  // 1 text row height incl. 2px spacing

// ----- Layout & spacing in PIXELS (supports "half-line" distances) -----
inline uint16_t rowHeightPx() { return LINE_STEP(); } // one text row height

// -------- Fine-grained spacing presets (scale with TEXT_SCALE) --------
#define PAD_NONE    0
#define PAD_QTR     (rowHeightPx() / 4)                  // quarter-line
#define PAD_HALF    (rowHeightPx() / 2)                  // half-line
#define PAD_3QTR    ((rowHeightPx() * 3) / 4)            // three-quarter line
#define PAD_FULL    (rowHeightPx())                      // full line

// -------- Global top margin over everything --------
#ifndef TOP_MARGIN_PX
  #define TOP_MARGIN_PX PAD_HALF                         // smaller top margin
#endif

// -------- Status area padding & separator --------
#ifndef STATUS_TOP_PADDING_PX
  #define STATUS_TOP_PADDING_PX PAD_HALF                 // space above status line
#endif
#ifndef STATUS_SEPARATOR_ENABLE
  #define STATUS_SEPARATOR_ENABLE 0                      // 1 = draw separator line
#endif
#ifndef STATUS_SEPARATOR_THICKNESS
  #define STATUS_SEPARATOR_THICKNESS 0                   // hairline thickness
#endif
#ifndef STATUS_SEPARATOR_INSET_PX
  #define STATUS_SEPARATOR_INSET_PX 0                    // left/right inset
#endif

// -------- Header (table title) fine paddings --------
#ifndef HEADER_TOP_PADDING_PX
  #define HEADER_TOP_PADDING_PX PAD_NONE                  // padding above header
#endif
#ifndef HEADER_BOTTOM_PADDING_PX
  #define HEADER_BOTTOM_PADDING_PX PAD_QTR              // padding below header
#endif
#ifndef HEADER_SEPARATOR_ENABLE
  #define HEADER_SEPARATOR_ENABLE 0                      // 1 = draw separator below header
#endif
#ifndef HEADER_SEPARATOR_THICKNESS
  #define HEADER_SEPARATOR_THICKNESS 1
#endif
#ifndef HEADER_SEPARATOR_INSET_PX
  #define HEADER_SEPARATOR_INSET_PX 0
#endif

// -------- Best-block fine paddings --------
#ifndef BEST_BLOCK_TOP_PADDING_PX
  #define BEST_BLOCK_TOP_PADDING_PX PAD_HALF             // space before BEST title
#endif
#ifndef BEST_TITLE_BOTTOM_PADDING_PX
  #define BEST_TITLE_BOTTOM_PADDING_PX PAD_QTR           // space under BEST title
#endif
#ifndef BEST_LINES_GAP_PX
  #define BEST_LINES_GAP_PX PAD_NONE                     // space between best lines
#endif
#ifndef BEST_BLOCK_SEPARATOR_ENABLE
  #define BEST_BLOCK_SEPARATOR_ENABLE 0                  // 1 = draw separator above best block
#endif
#ifndef BEST_BLOCK_SEPARATOR_THICKNESS
  #define BEST_BLOCK_SEPARATOR_THICKNESS 0
#endif
#ifndef BEST_BLOCK_SEPARATOR_INSET_PX
  #define BEST_BLOCK_SEPARATOR_INSET_PX 0
#endif


// ============================================================================
// ============================ STATUS LINE STATE ==============================
// ============================================================================
//
// A bottom status bar displaying WiFi and MQTT states.
//

static String wifiStatus = "";      // e.g., "WiFi: scanning...", "WiFi: SSID (-62 dBm)"
static String mqttStatus = "";      // e.g., "MQTT: reconnecting...", "MQTT: OK"
static bool   statusDirty = false;  // set true to trigger a partial redraw

inline void setWifiStatus(const String& s)  { if (wifiStatus != s) { wifiStatus = s; statusDirty = true; } }
inline void setMqttStatus(const String& s)  { if (mqttStatus != s) { mqttStatus = s; statusDirty = true; } }

// Build combined status string: "WiFi: ... | MQTT: ..."
String buildStatusLine() {
  String s;
  if (wifiStatus.length()) s += wifiStatus;
  if (mqttStatus.length()) {
    if (s.length()) s += " | ";
    s += mqttStatus;
  }
  return s;
}


// ============================================================================
// ========================== STRING UTILITY HELPERS ===========================
// ============================================================================

// Left-pad to a fixed width; truncate if too long.
String rightAlign(const String& s, uint8_t width) {
  String out = s;
  while (out.length() < width) out = " " + out;
  if (out.length() > width) out.remove(width);
  return out;
}


// ============================================================================
// ====================== CALLSIGN → ISO‑3 COUNTRY LOOKUP ======================
// ============================================================================

String getCountryIso3(const char* call) {
  if (!call || !call[0]) return "---";

  String c = call;
  c.toUpperCase();

  String px;
  for (char ch : c) {
    if (ch >= '0' && ch <= '9') break;
    if (ch >= 'A' && ch <= 'Z') px += ch;
  }
  if (!px.length()) return "---";

  for (uint16_t i=0; i<dxccCountryTableSize; i++) {
    const DxccCountry* e = &dxccCountryTable[i];
    if (px.startsWith(e->prefix))
      return String(e->iso3);
  }
  return "---";
}


// ============================================================================
// ========================= FREQUENCY → HAM BAND ==============================
// ============================================================================

String getBandRegion1(long f) {
  if (f>=135700   && f<=137800)   return "2200m";
  if (f>=472000   && f<=479000)   return "630m";
  if (f>=1810000  && f<=2000000)  return "160m";
  if (f>=3500000  && f<=3800000)  return "80m";
  if (f>=5351500  && f<=5366500)  return "60m";
  if (f>=7000000  && f<=7200000)  return "40m";
  if (f>=10100000 && f<=10150000) return "30m";
  if (f>=14000000 && f<=14350000) return "20m";
  if (f>=18068000 && f<=18168000) return "17m";
  if (f>=21000000 && f<=21450000) return "15m";
  if (f>=24890000 && f<=24990000) return "12m";
  if (f>=28000000 && f<=29700000) return "10m";
  if (f>=50000000 && f<=52000000) return "6m";
  if (f>=70000000 && f<=70500000) return "4m";
  if (f>=144000000 && f<=146000000) return "2m";
  if (f>=430000000 && f<=440000000) return "70cm";
  if (f>=1240000000 && f<=1300000000) return "23cm";
  return "OOB";
}

// Make sure band string is exactly 4 chars (pad/truncate).
String formatBand4(const String& b) {
  String x = b;
  while (x.length() < 4) x = " " + x;
  if (x.length() > 4) x = x.substring(0,4);
  return x;
}


// ============================================================================
// ================= MAIDENHEAD LOCATOR → GEO COORDINATES ======================
// ============================================================================

bool locatorToLatLon(const String& loc, float& lat, float& lon) {
  if (loc.length() < 4) return false;

  char A = toupper(loc[0]);
  char B = toupper(loc[1]);
  char C = loc[2];
  char D = loc[3];

  if (A<'A'||A>'R') return false;
  if (B<'A'||B>'R') return false;
  if (C<'0'||C>'9') return false;
  if (D<'0'||D>'9') return false;

  lon = (A - 'A') * 20 - 180 + (C - '0') * 2 + 1.0;
  lat = (B - 'A') * 10 - 90  + (D - '0') * 1 + 0.5;

  return true;
}


// ============================================================================
// ======================= HAVERSINE DISTANCE (KM) =============================
// ============================================================================

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0;
  float dLat = radians(lat2 - lat1);
  float dLon = radians(lon2 - lon1);

  float a = sin(dLat/2)*sin(dLat/2)
          + cos(radians(lat1))*cos(radians(lat2))
          * sin(dLon/2)*sin(dLon/2);

  return 2 * R * atan2(sqrt(a), sqrt(1-a));
}


// ============================================================================
// =========================== TIME FORMATTER UTC ==============================
// ============================================================================
//
// PSKReporter provides timestamps in UTC (epoch seconds). We only need to
// format them. No NTP required. IMPORTANT: use correct time_t conversion.
//

void formatUtcTime(long ts, char* out, size_t n) {
  if (ts <= 0) { strncpy(out, "--:--", n); return; }

  time_t t = (time_t)ts;       // Correct conversion to time_t
  struct tm* g = gmtime(&t);   // Correct pointer type

  if (!g) { strncpy(out, "--:--", n); return; }

  snprintf(out, n, "%02d:%02d", g->tm_hour, g->tm_min);
}


// ============================================================================
// =================== BUILD ONE FORMATTED DISPLAY LINE ========================
// ============================================================================

String buildOneLine(
  const char* utc,
  const String& band4,
  const char* mode5R,
  const char* snrRight,
  const String& rxLoc,
  const char* distRight,
  const char* ctry4R,
  const char* receiver )
{
  String s;
  s.reserve(96);

  s += utc; s += " ";
  s += band4; s += " ";
  s += mode5R; s += " ";
  s += snrRight; s += "  ";
  s += rxLoc; s += " ";
  s += distRight; s += " ";
  s += ctry4R; s += "  ";
  s += receiver;

  uint16_t maxChars = display.width() / charW();
  if (s.length() > maxChars) s.remove(maxChars);

  return s;
}


// ============================================================================
// ============================== SPLASH SCREEN ================================
// ============================================================================

void showSplash() {
  display.setRotation(1);
  display.setFont(NULL);

  const uint8_t splashScale = 2;
  display.setTextSize(splashScale);
  display.setFullWindow();

  const char *line1 = "DL8UG";
  const char *line2 = "POCKET-REPORTER";

  const int cw = 6 * splashScale;
  const int ch = 8 * splashScale;

  const int w1 = strlen(line1) * cw;
  const int w2 = strlen(line2) * cw;

  const int x1 = (display.width() - w1) / 2;
  const int x2 = (display.width() - w2) / 2;

  const int lineGap = 3 * splashScale;
  const int blockH  = ch + lineGap + ch;
  const int top     = (display.height() - blockH) / 2;

  const int baseline1 = top + ch - 12;
  const int baseline2 = baseline1 + lineGap + ch;

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    display.setCursor(x1, baseline1);
    display.print(line1);

    display.setCursor(x2, baseline2);
    display.print(line2);

  } while (display.nextPage());

  delay(2500);
}


// ============================================================================
// ============ AUTO-FIT COMPUTATION FOR HISTORY LINES (PIXEL-BASED) ==========
// ============================================================================

// Compute how many history rows fit between the header area and the best-block,
// while respecting the reserved status area at the bottom.
// Returns clamped value between 1 and MAX_LINES_HISTORY.
uint8_t computeAutoHistoryLines() {
  // screen metrics
  const uint16_t scrH = display.height();

  const uint16_t rowH        = rowHeightPx();
  const uint16_t topMarginPx = TOP_MARGIN_PX;
  const uint16_t statusPadPx = STATUS_TOP_PADDING_PX;

  // Bottom status text baseline sits at (scrH - charH()).
  const uint16_t yBottom       = scrH - charH();
  const uint16_t statusAreaTop = (uint16_t)(yBottom - statusPadPx);

  // Start below top margin
  uint16_t y = topMarginPx;

  // Header block height:
  y += HEADER_TOP_PADDING_PX;
  y += rowH; // header text row
  y += HEADER_BOTTOM_PADDING_PX;

#if HEADER_SEPARATOR_ENABLE
  // separator right below the header
  y += HEADER_SEPARATOR_THICKNESS;
#endif

  // Best block required height (we must keep this space free at the bottom of the usable area)
  const uint16_t bestBlockRequired =
    BEST_BLOCK_TOP_PADDING_PX
    + rowH                        // best title
    + BEST_TITLE_BOTTOM_PADDING_PX
    + rowH                        // bestDistLine
    + BEST_LINES_GAP_PX
    + rowH;                       // bestSnrLine

  const uint16_t usableHeight = (statusAreaTop > bestBlockRequired)
                              ? (statusAreaTop - bestBlockRequired)
                              : 0;

  if (usableHeight <= y) return 1;  // ensure at least 1 row
  uint16_t freePx = usableHeight - y;
  uint8_t possibleRows = (uint8_t)(freePx / rowH);

  if (possibleRows < 1) possibleRows = 1;
  if (possibleRows > MAX_LINES_HISTORY) possibleRows = MAX_LINES_HISTORY;
  return possibleRows;
}


// ============================================================================
// ======================= RENDER ENTIRE SCREEN (PARTIAL) ======================
// ============================================================================
//
// Draws:
//  (A) Table header
//  (B) History list
//  (C) Best distance / Best SNR
//  (D) Optional separator above status
//  (E) Bottom status bar (WiFi | MQTT)
//  Layout uses pixel-based spacing (supports half-line gaps).
//

void renderContentPartial() {
  display.setRotation(1);
  display.setFont(NULL);
  display.setTextSize(TEXT_SCALE);

  display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    display.fillRect(0, 0, display.width(), display.height(), GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    const uint16_t rowH        = rowHeightPx();
    const uint16_t topMarginPx = TOP_MARGIN_PX;
    const uint16_t statusPadPx = STATUS_TOP_PADDING_PX;

    // Bottom status text baseline (last text row)
    const uint16_t yBottom = display.height() - charH();

    // Top boundary of status area (padding sits above the status text baseline)
    const uint16_t statusAreaTop = (uint16_t)(yBottom - statusPadPx);

    // Main content may not cross into status area
    const uint16_t usableHeight = statusAreaTop;

    uint16_t y = topMarginPx;

    // ---- (A) TABLE HEADER (column labels) ----
    y += HEADER_TOP_PADDING_PX;                    // fine padding above header
    display.setCursor(0, y);
    display.print(headerLine);
    y += rowH;                                     // header row height
    y += HEADER_BOTTOM_PADDING_PX;                 // fine padding below header

#if HEADER_SEPARATOR_ENABLE
    {
      // draw hairline separator right below the header block
      uint16_t sepY = (y > HEADER_SEPARATOR_THICKNESS) ? (y - 1) : y;
      uint16_t sepX = HEADER_SEPARATOR_INSET_PX;
      uint16_t sepW = display.width() - (2 * HEADER_SEPARATOR_INSET_PX);
      if (sepW > 0) {
        display.fillRect(sepX, sepY, sepW, HEADER_SEPARATOR_THICKNESS, GxEPD_BLACK);
      }
    }
#endif

    // ---- (B) HISTORY LIST ----
    for (int i = 0; i < linesHistory; i++) {
      if (y + rowH > usableHeight) break;         // avoid overlapping status area
      display.setCursor(0, y);
      display.print(lines[i]);
      y += rowH;
    }

    // ---- Optional separator or fine padding before BEST block ----
#if BEST_BLOCK_SEPARATOR_ENABLE
    {
      uint16_t sepY = (y + BEST_BLOCK_TOP_PADDING_PX > BEST_BLOCK_SEPARATOR_THICKNESS)
                      ? (y + BEST_BLOCK_TOP_PADDING_PX - 1)
                      : (y);
      uint16_t sepX = BEST_BLOCK_SEPARATOR_INSET_PX;
      uint16_t sepW = display.width() - (2 * BEST_BLOCK_SEPARATOR_INSET_PX);
      if (sepW > 0 && sepY < usableHeight) {
        display.fillRect(sepX, sepY, sepW, BEST_BLOCK_SEPARATOR_THICKNESS, GxEPD_BLACK);
      }
    }
#endif
    y += BEST_BLOCK_TOP_PADDING_PX;               // fine padding before BEST title

    // ---- (C) BEST BLOCK ----
    if (y + rowH <= usableHeight) {
      display.setCursor(0, y);
      display.print("BEST DIST / SNR");
      y += rowH;
      y += BEST_TITLE_BOTTOM_PADDING_PX;          // fine padding below title
    }

    if (y + rowH <= usableHeight) {
      display.setCursor(0, y);
      display.print(bestDistLine);
      y += rowH;
    }

    y += BEST_LINES_GAP_PX;                       // fine gap between best lines

    if (y + rowH <= usableHeight) {
      display.setCursor(0, y);
      display.print(bestSnrLine);
    }

    // ---- (D) Optional separator above status area ----
#if STATUS_SEPARATOR_ENABLE
    {
      uint16_t sepY = (statusAreaTop > STATUS_SEPARATOR_THICKNESS)
                      ? (statusAreaTop - STATUS_SEPARATOR_THICKNESS) : 0;
      uint16_t sepX = STATUS_SEPARATOR_INSET_PX;
      uint16_t sepW = display.width() - (2 * STATUS_SEPARATOR_INSET_PX);
      if (sepW > 0) {
        display.fillRect(sepX, sepY, sepW, STATUS_SEPARATOR_THICKNESS, GxEPD_BLACK);
      }
    }
#endif

    // ---- (E) STATUS BAR (always last line) ----
    {
      String st = buildStatusLine();
      uint16_t maxChars = display.width() / charW();
      if (st.length() > maxChars) st.remove(maxChars);

      display.setCursor(0, yBottom);
      display.print(st);
    }

  } while (display.nextPage());
}


// ============================================================================
// ========================= WIFI INITIALIZATION ===============================
// ============================================================================

void setupWiFiBasics() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // helpful when AP temporarily drops
  WiFi.persistent(false);       // do not write creds to flash on begin()
}

// Find index of SSID in wifiList; returns -1 if not present
int findWifiIndexBySsid(const String& ssid) {
  for (int i = 0; i < wifiCount; i++) {
    if (ssid == wifiList[i].ssid) return i;
  }
  return -1;
}

// Load last used SSID index from NVS; -1 if none or not found in list
int loadLastUsedWifi() {
  String last = prefs.getString(PREF_KEY, "");
  if (last.length() == 0) return -1;
  return findWifiIndexBySsid(last);
}

// Save last used SSID to NVS (called on successful connect)
void saveLastUsedWifi(const char* ssid) {
  if (ssid && ssid[0]) {
    prefs.putString(PREF_KEY, ssid);
    Serial.printf("Saved last SSID to NVS: %s\n", ssid);
  }
}

// Try to connect to a specific WiFi entry; return true on success
bool connectToEntry(const WifiEntry& e, uint32_t timeoutMs) {
  setWifiStatus("WiFi: connecting " + String(e.ssid));

  WiFi.begin(e.ssid, e.password);

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      int rssi = WiFi.RSSI();
      setWifiStatus("WiFi: " + String(e.ssid) + " (" + String(rssi) + " dBm)");
      saveLastUsedWifi(e.ssid);
      return true;
    }
    delay(250);
  }
  setWifiStatus("WiFi: connect timeout");
  return false;
}

// Scan for known networks; connect to the strongest one
bool connectBestKnown(uint32_t timeoutMs) {
  setWifiStatus("WiFi: scanning...");

  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  if (n <= 0) {
    setWifiStatus("WiFi: no networks found");
    return false;
  }

  int bestIdxInList = -1;
  int bestRSSI = -999;

  for (int i = 0; i < n; i++) {
    String ssidScan = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);

    int idx = findWifiIndexBySsid(ssidScan);
    if (idx >= 0 && rssi > bestRSSI) {
      bestRSSI = rssi;
      bestIdxInList = idx;
    }
  }

  if (bestIdxInList < 0) {
    setWifiStatus("WiFi: known SSIDs not found");
    return false;
  }

  setWifiStatus("WiFi: connecting " + String(wifiList[bestIdxInList].ssid) +
                " (" + String(bestRSSI) + " dBm)");

  return connectToEntry(wifiList[bestIdxInList], timeoutMs);
}

/**
 * Ensure WiFi connection using:
 *  1) Fast path: try last known SSID from NVS (short timeout)
 *  2) Fallback: scan and connect to the strongest known network
 */
bool ensureWiFi(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  setWifiStatus("WiFi: reconnecting...");
  WiFi.disconnect(true, true);
  delay(200);

  // Fast path: last used SSID (≈40% of total timeout, capped to 6s)
  uint32_t fastTryMs = (timeoutMs * 2 / 5);
  if (fastTryMs > 6000) fastTryMs = 6000;

  int lastIdx = loadLastUsedWifi();
  if (lastIdx >= 0) {
    setWifiStatus("WiFi: trying last SSID " + String(wifiList[lastIdx].ssid));
    if (connectToEntry(wifiList[lastIdx], fastTryMs)) return true;
  } else {
    setWifiStatus("WiFi: no last SSID; scanning...");
  }

  // Fallback: scan & connect to strongest known SSID
  uint32_t remaining = (timeoutMs > fastTryMs) ? (timeoutMs - fastTryMs) : 5000;
  return connectBestKnown(remaining);
}


// ============================================================================
// ========================= MQTT CONNECTION LOGIC =============================
// ============================================================================

bool connectMQTTOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String cid = "ESP32_CALL_";
  cid += TRACK_CALL;
  cid += "_";
  cid += String(random(0xffff), HEX);

  if (client.connect(cid.c_str())) {
    client.subscribe(topic_call.c_str());
    return true;
  }
  return false;
}

// MQTT reconnection with exponential backoff; requires WiFi first
void reconnectMQTT() {
  static unsigned long nextTry = 0;
  static uint8_t fails = 0;

  if (client.connected()) return;
  if (millis() < nextTry) return;

  if (!ensureWiFi(15000)) {
    setMqttStatus("MQTT: waiting for WiFi…");
    uint8_t shift = (fails > 4) ? 4 : fails;
    nextTry = millis() + (2000UL << shift);
    fails++;
    return;
  }

  setMqttStatus("MQTT: reconnecting…");
  if (connectMQTTOnce()) {
    setMqttStatus("MQTT: OK");
    fails = 0;
    nextTry = millis() + 5000;
    return;
  }

  setMqttStatus("MQTT: retrying…");
  uint8_t shift = (fails > 4) ? 4 : fails;
  nextTry = millis() + (2000UL << shift);
  fails++;
}


// ============================================================================
// ============================== MQTT CALLBACK ================================
// ============================================================================
//
// Processes each PSKReporter spot (JSON):
//  - Parse JSON
//  - Format fields
//  - Compute Maidenhead distance
//  - Build line
//  - Track best distance/SNR
//  - Redraw screen
//

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // Raw payload -> String
  String msg;
  msg.reserve(length);
  for (unsigned int i=0; i<length; i++)
    msg += (char)payload[i];

  // Parse JSON
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, msg)) return;

  const char* receiverCall = doc["rc"] | "???";
  long freq = doc["f"] | 0;
  int  snr  = doc["rp"] | -999;
  long ts   = doc["ts"] | doc["t"] | 0;

  // Mode padded to 5 chars
  String mode5 = String(doc["md"] | "?");
  while (mode5.length() < 5) mode5 = " " + mode5;

  // Sender/receiver locator (4 chars)
  String sg = String(doc["sl"] | "????").substring(0,4);
  String rg = String(doc["rl"] | "????").substring(0,4);

  // Distance
  float lat1, lon1, lat2, lon2;
  float dist = -1;
  if (locatorToLatLon(sg, lat1, lon1) &&
      locatorToLatLon(rg, lat2, lon2))
    dist = distanceKm(lat1, lon1, lat2, lon2);

  String band4 = formatBand4(getBandRegion1(freq));

  // Time formatting (UTC from PSKR, no NTP)
  char utc[8];
  formatUtcTime(ts, utc, sizeof(utc));

  // SNR formatting
  char snrBuf[8];
  if (snr == -999) strcpy(snrBuf, " n/a");
  else snprintf(snrBuf, sizeof(snrBuf), "%4d", snr);

  // Distance formatting
  char distBuf[8];
  if (dist > 0) snprintf(distBuf, sizeof(distBuf), "%5.0f", dist);
  else strncpy(distBuf, "    -", sizeof(distBuf));

  // Country via DXCC
  String country3 = getCountryIso3(receiverCall);
  String ctry4    = rightAlign(country3, 4);

  // Build output line
  String line = buildOneLine(
    utc, band4, mode5.c_str(),
    snrBuf, rg, distBuf,
    ctry4.c_str(), receiverCall
  );

  // Scroll history in fixed buffer size, limited by linesHistory
  for (int i = (int)linesHistory - 1; i > 0; i--)
    lines[i] = lines[i-1];
  lines[0] = line;

  // Update best distance
  if (dist > bestDistVal) {
    bestDistVal = dist;
    bestDistLine = line;
  }

  // Update best SNR
  if (snr != -999 && snr > bestSnrVal) {
    bestSnrVal = snr;
    bestSnrLine = line;
  }

  // Redraw
  renderContentPartial();
}


// ============================================================================
// ============================ SETUP ROUTINE =================================
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(300);

#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
  // Initialize HSPI with Waveshare board pinout:
  // hspi.begin(SCK=13, MISO=12, MOSI=14, SS=15)
  hspi.begin(13, 12, 14, 15);
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#endif

  display.init(115200);
  showSplash();

  // Clear screen once
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());

  // Decide how many history rows to use: fixed from config or auto-fit
  if (HISTORY_LINES > 0) {
    linesHistory = (HISTORY_LINES > MAX_LINES_HISTORY)
                   ? MAX_LINES_HISTORY
                   : (uint8_t)HISTORY_LINES;
  } else {
    // Auto-fit based on current layout and screen size
    linesHistory = computeAutoHistoryLines();
  }

  // Reset buffers (full buffer to be safe)
  for (int i = 0; i < MAX_LINES_HISTORY; i++) lines[i] = "";
  bestDistLine = "";
  bestSnrLine  = "";

  // Initial status
  setWifiStatus("WiFi: boot...");
  setMqttStatus("MQTT: idle");
  renderContentPartial();

  // Build PSKReporter MQTT topic with callsign filter
  topic_call  = "pskr/filter/v2/+/+/";
  topic_call += TRACK_CALL;
  topic_call += "/#";

  // WiFi init + NVS for last SSID cache
  setupWiFiBasics();
  if (!prefs.begin(PREF_NS, /*readOnly=*/false)) {
    Serial.println("Failed to open NVS (wifi namespace).");
  }
  ensureWiFi(15000);

  // MQTT init
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(1024);
  client.setKeepAlive(30);
  client.setSocketTimeout(5);
}


// ============================================================================
// ================================= LOOP =====================================
// ============================================================================

void loop() {

  // Keep WiFi alive / reconnect if needed (tries last SSID, then best known)
  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFi(15000);
    // ensureWiFi() updates WiFi status strings
  } else {
    // Optional: refresh WiFi status occasionally to reflect current RSSI
    static unsigned long nextWiFiStatus = 0;
    if (millis() > nextWiFiStatus) {
      setWifiStatus("WiFi: " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)");
      nextWiFiStatus = millis() + 15000; // update every 15s
    }
  }

  // If status changed, re-render bottom line (and all content) using partial update
  if (statusDirty) {
    statusDirty = false;
    renderContentPartial();
  }

  // Maintain MQTT connection
  if (!client.connected()) {
    reconnectMQTT();
  } else {
    client.loop();
  }

  delay(50); // keep loop gentle
}