#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// USER CONFIGURATION
// ---------------------------------------------------------------------------

// Callsign to filter in the PSKReporter MQTT feed
#define TRACK_CALL "RA3VMK"

// Built‑in 6×8 font text scale (1..3)
#define TEXT_SCALE 1

// ---------------------------------------------------------------------------
// MULTI‑WIFI LIST (EDIT ME)
// ---------------------------------------------------------------------------
// Add your known WiFi networks here. The sketch will:
//  - try the last used SSID first (cached in NVS)
//  - if that fails, scan and connect to the strongest known SSID
// Order doesn't imply priority; strongest RSSI wins if last used fails.
//
struct WifiEntry {
    const char* ssid;
    const char* password;
};

static const WifiEntry wifiList[] = {
    { "SSID-A",  "PASS-A" },
    { "SSID-B",  "PASS-B" },
};

static const int wifiCount = sizeof(wifiList) / sizeof(wifiList[0]);


// ---------------------------------------------------------------------------
// HISTORY LINES (report rows)
// ---------------------------------------------------------------------------
// Set to a positive number (e.g., 4, 6, 8, 10) to force a fixed history length.
// Set to 0 (or leave undefined) to auto-fit based on screen height & paddings.
#ifndef HISTORY_LINES
  #define HISTORY_LINES 6   // 0 = auto-fit, >0 = fixed amount
#endif

// Upper bound for the history buffer (must cover any possible HISTORY_LINES)
#ifndef MAX_LINES_HISTORY
  #define MAX_LINES_HISTORY 12
#endif

#endif // CONFIG_H