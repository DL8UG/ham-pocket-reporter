/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <mail@dl8ug.de> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return. 73, DL8UG - Uwe
 * ----------------------------------------------------------------------------
 */

/*
 * Waveshare 2.9 inch 
 * BUSY → GPIO 25
 * RST  → GPIO 26
 * DC   → GPIO 27
 * CS   → GPIO 15
 * CLK  → GPIO 13
 * DIN  → GPIO 14
*/


#include "config.h"               // Contains TRACK_CALL, WiFi credentials
#include <GxEPD2_BW.h>            // E‑paper display driver
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include "dxcc_country_table.h"   // DXCC prefix → ISO‑3 country map



// ============================================================================
// ========================= DISPLAY CONFIGURATION ============================
// ============================================================================
//
// This block contains all hardware‑specific settings for the 2.9" Waveshare
// B/W V2 e‑paper panel. The display runs in partial refresh mode to minimize
// ghosting and reduce update times.
//

#define USE_HSPI_FOR_EPD           // Use HSPI bus instead of default VSPI
#define ENABLE_GxEPD2_GFX 0        // Disable GxEPD2_GFX (saves RAM & flash)

// Display class/driver selection
#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS  GxEPD2_290_T94_V2

// ESP32-specific: calculate max buffer height dynamically
#if defined(ESP32)
  #define MAX_DISPLAY_BUFFER_SIZE 65536ul
  #define MAX_HEIGHT(EPD) \
      (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) \
      ? EPD::HEIGHT \
      : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))
#endif

// Construct display object with pin configuration (CS=15, DC=27, RST=26, BUSY=25)
GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)>
  display(GxEPD2_DRIVER_CLASS(15, 27, 26, 25));

#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
SPIClass hspi(HSPI);              // Dedicated HSPI instance
#endif



// ============================================================================
// ============================== WIFI & MQTT =================================
// ============================================================================
//
// Handles WiFi connection, reconnection logic, MQTT subscription,
// and incoming PSKReporter JSON spot messages.
//

const char* mqtt_server = "mqtt.pskreporter.info";
const int   mqtt_port   = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// Will contain: "pskr/filter/v2/+/+/<CALLSIGN>/#"
String topic_call;



// ============================================================================
// ========================= DISPLAY RUNTIME STATE =============================
// ============================================================================
//
// These variables hold the scrolling spot history, as well as the best
// distance and best SNR values seen so far.
//

#define LINES_HISTORY 6

static String lines[LINES_HISTORY];   // Circular spot log
static String bestDistLine = "";      // Best-distance line (formatted)
static String bestSnrLine  = "";      // Best-SNR line (formatted)

static float  bestDistVal  = -1;      // Best distance value
static int    bestSnrVal   = -999;    // Best SNR value

String headerLine = "UTC   BAND  MODE  SNR   LOC  DIST  ISO  RX";



// ============================================================================
// ======================== TEXT FONT SIZE & HELPERS ===========================
// ============================================================================
//
// The display uses a built-in 6×8 pixel font. All geometry calculations
// derive from TEXT_SCALE.
//

#define TEXT_SCALE 1

inline uint8_t  charW() { return 6 * TEXT_SCALE; }
inline uint8_t  charH() { return 8 * TEXT_SCALE; }
inline uint16_t LINE_STEP() { return charH() + 2; }

inline uint16_t TOP_Y() { return charH() + 2; }



// ============================================================================
// ========================== SMALL STRING UTILITIES ===========================
// ============================================================================

// Pads a string on the left so it becomes exactly <width> characters.
String rightAlign(const String& s, uint8_t width) {
  String out = s;
  while (out.length() < width) out = " " + out;
  if (out.length() > width) out.remove(width);
  return out;
}



// ============================================================================
// ====================== CALLSIGN → ISO‑3 COUNTRY LOOKUP ======================
// ============================================================================
//
// Extracts callsign prefix (letters until first digit). Uses DXCC lookup table.
//

String getCountryIso3(const char* call) {
  if (!call || !call[0]) return "---";

  // Normalize to uppercase
  String c = call;
  c.toUpperCase();

  // Extract prefix (only A–Z until digit)
  String px;
  for (char ch : c) {
    if (ch >= '0' && ch <= '9') break;
    if (ch >= 'A' && ch <= 'Z') px += ch;
  }
  if (!px.length()) return "---";

  // Search DXCC table (startsWith matching)
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
//
// Maps PSKReporter frequency (Hz) to standard ITU Region 1 band names.
//

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

// Force band string to always be 4 chars wide
String formatBand4(const String& b) {
  String x = b;
  while (x.length() < 4) x = " " + x;
  if (x.length() > 4) x = x.substring(0,4);
  return x;
}



// ============================================================================
// ================= MAIDENHEAD LOCATOR → GEO COORDINATES ======================
// ============================================================================
//
// Converts 4-char Maidenhead locator (e.g. JO31) to approximate latitude &
// longitude of the center of the grid square.
//

bool locatorToLatLon(const String& loc, float& lat, float& lon) {
  if (loc.length() < 4) return false;

  char A = toupper(loc[0]);
  char B = toupper(loc[1]);
  char C = loc[2];
  char D = loc[3];

  // Validate characters
  if (A<'A'||A>'R') return false;
  if (B<'A'||B>'R') return false;
  if (C<'0'||C>'9') return false;
  if (D<'0'||D>'9') return false;

  // Convert grid to lat/lon
  lon = (A - 'A') * 20 - 180 + (C - '0') * 2 + 1.0;
  lat = (B - 'A') * 10 - 90  + (D - '0') * 1 + 0.5;

  return true;
}



// ============================================================================
// ======================= HAVERSINE DISTANCE (KM) =============================
// ============================================================================
//
// Computes great‑circle distance using the standard Haversine formula.
//

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0;  // Earth radius in km
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
// Converts a UNIX timestamp to "HH:MM" (UTC). Falls back to "--:--".
//

void formatUtcTime(long ts, char* out, size_t n)
{
    if (ts <= 0) {
        strncpy(out, "--:--", n);
        return;
    }

    // CORRECT conversion type!
    time_t t = (time_t)ts;
    struct tm* g = gmtime(&t);

    if (!g) {
        strncpy(out, "--:--", n);
        return;
    }

    snprintf(out, n, "%02d:%02d", g->tm_hour, g->tm_min);
}



// ============================================================================
// =================== BUILD ONE FORMATTED DISPLAY LINE ========================
// ============================================================================
//
// Creates the full single-line display string containing:
//
// UTC | BAND | MODE | SNR | LOC | DIST | ISO | CALLSIGN
//
// The line is clipped to the display width.
//

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

  // Clip to display width
  uint16_t maxChars = display.width() / charW();
  if (s.length() > maxChars) s.remove(maxChars);

  return s;
}



// ============================================================================
// ============================== SPLASH SCREEN ================================
// ============================================================================
//
// A centered 2-line splash ("DL8UG" / "REPORTER") shown at boot.
//

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
// ======================= RENDER ENTIRE SCREEN (PARTIAL) ======================
// ============================================================================
//
// Redraws all content using partial refresh:
//
// 1) Header
// 2) Spot history
// 3) Best distance & best SNR
//
// Partial refresh is fast and avoids full-screen flashing.
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

    uint16_t y = TOP_Y();

    // 1) Header
    display.setCursor(0, y);
    display.print(headerLine);
    y += LINE_STEP();

    // 2) History lines
    for (int i = 0; i < LINES_HISTORY; i++) {
      display.setCursor(0, y);
      display.print(lines[i]);
      y += LINE_STEP();
    }

    y += LINE_STEP();

    // 3) Best stats header
    display.setCursor(0, y);
    display.print("BEST DIST / SNR");
    y += LINE_STEP();

    // 4) Best distance
    display.setCursor(0, y);
    display.print(bestDistLine);
    y += LINE_STEP();

    // 5) Best SNR
    display.setCursor(0, y);
    display.print(bestSnrLine);

  } while (display.nextPage());
}



// ============================================================================
// ========================= WIFI INITIALIZATION ===============================
// ============================================================================

void setupWiFiBasics() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
}

// Attempt WiFi connection with timeout
bool ensureWiFi(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.disconnect(true, true);
  delay(200);
  WiFi.begin(ssid, password);

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(250);
  }
  return false;
}



// ============================================================================
// ========================= MQTT CONNECTION LOGIC =============================
// ============================================================================

// Connect once; on success, subscribe to callsign filter topic
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

// MQTT reconnection with exponential backoff
void reconnectMQTT() {
  static unsigned long nextTry = 0;
  static uint8_t fails = 0;

  if (client.connected()) return;
  if (millis() < nextTry) return;

  if (!ensureWiFi(15000)) {
    nextTry = millis() + (2000UL << min<uint8_t>(fails, 4));
    fails++;
    return;
  }

  if (connectMQTTOnce()) {
    fails = 0;
    nextTry = millis() + 5000;
    return;
  }

  nextTry = millis() + (2000UL << min<uint8_t>(fails, 4));
  fails++;
}



// ============================================================================
// ============================== MQTT CALLBACK ================================
// ============================================================================
//
// Processes each incoming PSKReporter spot:
//
//  - Parse JSON
//  - Format all fields
//  - Compute Maidenhead distance
//  - Build display string
//  - Update best stats
//  - Redraw screen
//

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // Convert raw bytes to string
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

  // Mode padded to fixed width
  String mode5 = String(doc["md"] | "?");
  while (mode5.length() < 5) mode5 = " " + mode5;

  // Sender/receiver locator (4 chars)
  String sg = String(doc["sl"] | "????").substring(0,4);
  String rg = String(doc["rl"] | "????").substring(0,4);

  // Distance calculation
  float lat1, lon1, lat2, lon2;
  float dist = -1;
  if (locatorToLatLon(sg, lat1, lon1) &&
      locatorToLatLon(rg, lat2, lon2))
    dist = distanceKm(lat1, lon1, lat2, lon2);

  String band4 = formatBand4(getBandRegion1(freq));

  char utc[8];
  formatUtcTime(ts, utc, sizeof(utc));

  char snrBuf[8];
  if (snr == -999) strcpy(snrBuf, " n/a");
  else snprintf(snrBuf, sizeof(snrBuf), "%4d", snr);

  char distBuf[8];
  if (dist > 0) snprintf(distBuf, sizeof(distBuf), "%5.0f", dist);
  else strncpy(distBuf, "    -", sizeof(distBuf));

  String country3 = getCountryIso3(receiverCall);
  String ctry4    = rightAlign(country3, 4);

  // Build final printable line
  String line = buildOneLine(
    utc, band4, mode5.c_str(),
    snrBuf, rg, distBuf,
    ctry4.c_str(), receiverCall
  );

  // Scroll history
  for (int i=LINES_HISTORY-1; i>0; i--)
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

  renderContentPartial();
}



// ============================================================================
// ============================ SETUP ROUTINE =================================
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(300);

#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
  // Initialize dedicated HSPI bus for the e-paper display
  hspi.begin(13, 12, 14, 15);
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#endif

  display.init(115200);    // Initialize e-paper

  showSplash();            // Show DL8UG REPORTER splash screen

  // Clear screen once
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());

  // Clear runtime buffers
  for (int i=0; i<LINES_HISTORY; i++) lines[i] = "";
  bestDistLine = "";
  bestSnrLine  = "";

  renderContentPartial();  // Draw initial empty screen

  // Build MQTT filter topic
  topic_call  = "pskr/filter/v2/+/+/";
  topic_call += TRACK_CALL;
  topic_call += "/#";

  // WiFi
  setupWiFiBasics();
  ensureWiFi(15000);

  // MQTT
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

  // Ensure WiFi is always connected
  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFi(15000);   // Try reconnecting WiFi
    delay(100);          // Small delay to avoid tight loop
  }

  // Maintain MQTT connection
  if (!client.connected()) {
    reconnectMQTT();
  } else {
    client.loop();       // Process incoming MQTT messages
  }
}
