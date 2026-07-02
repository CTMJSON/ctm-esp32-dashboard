/*
  CTM Activity Dashboard  -  ESP32-2432S028R (Cheap Yellow Display)
  ---------------------------------------------------------------
  Board: ESP32-D0WD-V3 + built-in 2.8" 320x240 ILI9341 SPI TFT.
  No external wiring required - display is hard-wired on the board.
  Display config lives in TFT_eSPI/User_Setup.h (CYD pins, TFT_RST=12).

  Authentication: OAuth2 Device Flow
    On first boot (or after token expiry), the device shows a user code
    and verification URL on the TFT. The user visits the URL, enters the
    code, and authorizes the device. Tokens are stored in NVS (Preferences)
    and persist across reboots. Access tokens are refreshed automatically.

  Libraries (Arduino IDE Library Manager):
    TFT_eSPI, ArduinoJson v7+
  Board: "ESP32 Dev Module".

  Polls GET /accounts/{id}/calls/history.json?interval=today every 60
  seconds. Shows active calls, peak per-minute, and today's totals.
  HTTPS cert validation is skipped (WiFiClientSecure::setInsecure).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <time.h>
#include <stdlib.h>
#include "secrets.h"
#include "ctm_logo.h"

#define REFRESH_MS    60000UL
#define API_HOST      "api.calltrackingmetrics.com"

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// Draw the CTM logomark with transparency (skip 0x0000 pixels)
static void drawCtmLogo(int x, int y) {
  for (int row = 0; row < CTM_LOGO_H; row++) {
    for (int col = 0; col < CTM_LOGO_W; col++) {
      uint16_t px = pgm_read_word(&ctm_logo[row * CTM_LOGO_W + col]);
      if (px != 0x0000) tft.drawPixel(x + col, y + row, px);
    }
  }
}

// CTM brand palette (RGB565)
#define CTM_SKY_BLUE      0x05FE
#define CTM_NEBULA_BLUE   0x04B9
#define CTM_DARK_MATTER   0x0AF1
#define CTM_SPACE_NAVY    0x1149
#define CTM_LIME          0xD6C0
#define CTM_GALAXY_GREY   0x3186

#define COL_HEADER   CTM_NEBULA_BLUE
#define COL_BG       CTM_SPACE_NAVY
#define COL_OK       CTM_LIME
#define COL_ERR      TFT_RED
#define COL_TEXT     TFT_WHITE
#define COL_DIM      TFT_WHITE
#define COL_TILE     CTM_DARK_MATTER
#define COL_ACTIVE   CTM_LIME
#define COL_IN       CTM_SKY_BLUE
#define COL_OUT      CTM_NEBULA_BLUE
#define COL_CHAT     CTM_LIME
#define COL_MISS     0xFD20
#define COL_VID      CTM_NEBULA_BLUE

// --- State machine ---------------------------------------------------------
enum AppState {
  STATE_BOOT,
  STATE_DEVICE_FLOW,
  STATE_DASHBOARD,
};
static AppState g_state = STATE_BOOT;

// --- OAuth token storage (NVS) ---------------------------------------------
static String g_accessToken = "";
static String g_refreshToken = "";
static String g_accountId = "";
static bool g_authFailed = false;

static void loadTokens() {
  prefs.begin("ctm", true);
  // Check token version - clear if mismatch (scope changes etc)
  uint32_t ver = prefs.getUInt("ver", 0);
  if (ver != 2) {
    prefs.end();
    clearTokens();
    prefs.begin("ctm", false);
    prefs.putUInt("ver", 2);
    prefs.end();
    return;
  }
  g_accessToken  = prefs.getString("access_tok", "");
  g_refreshToken = prefs.getString("refresh_tok", "");
  g_accountId    = prefs.getString("acct_id", "");
  prefs.end();
}

static void saveTokens(const String& at, const String& rt, const String& acct) {
  prefs.begin("ctm", false);
  prefs.putString("access_tok", at);
  prefs.putString("refresh_tok", rt);
  prefs.putString("acct_id", acct);
  prefs.end();
  g_accessToken = at;
  g_refreshToken = rt;
  g_accountId = acct;
}

static void clearTokens() {
  prefs.begin("ctm", false);
  prefs.remove("access_tok");
  prefs.remove("refresh_tok");
  prefs.remove("acct_id");
  prefs.end();
  g_accessToken = g_refreshToken = g_accountId = "";
}

static String bearerHeader() {
  return String("Bearer ") + g_accessToken;
}

// --- Device flow state ------------------------------------------------------
static String g_deviceCode;
static String g_userCode;
static String g_verifyUri;
static int g_pollInterval = 5;
static int g_flowExpiresIn = 1500;
static unsigned long g_flowStartMs = 0;
static unsigned long g_lastPollMs = 0;

// --- Dashboard metrics ------------------------------------------------------
static char g_posixTz[48] = "EST5EDT,M3.2.0,M11.1.0";
static bool g_ntpReady = false;
static bool g_firstOk = false;

static int g_active = 0, g_inbound = 0, g_outbound = 0;
static int g_chats = 0, g_missed = 0, g_videos = 0, g_total = 0;
static int g_peak = 0;
static int g_agentsReady = 0, g_agentsNotReady = 0, g_agentsTotal = 0;

// Recent call summaries for ticker
#define MAX_SUMMARIES 10
static String g_summaries[MAX_SUMMARIES];
static int g_summaryCount = 0;
static String g_tickerText = "";
static int g_tickerOffset = 0;

static char g_lastUpdated[9] = "--:--:--";
static char g_lastDate[20]   = "------";
static String g_status = "boot";

static const char* const DOW[] = {
  "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

struct TzMap { const char* iana; const char* posix; };
static const TzMap TZ_MAP[] = {
  {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Chicago",     "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Denver",      "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Phoenix",     "MST7"},
  {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Anchorage",   "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Pacific/Honolulu",    "HST10"},
  {"UTC",                 "UTC0"},
};
static const char* posixFor(const char* iana) {
  for (const auto& t : TZ_MAP)
    if (strcasecmp(t.iana, iana) == 0) return t.posix;
  return nullptr;
}

// --- Boot / WiFi / NTP ------------------------------------------------------

static void showBoot(const String& s) {
  g_status = s;
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print(F("CTM Dashboard"));
  tft.setTextSize(1);
  tft.setTextColor(0x7BEF);
  tft.setCursor(10, 40);
  tft.print(F("ESP32-2432S028R (CYD)"));
  tft.setTextColor(COL_TEXT);
  tft.setCursor(10, 70);
  tft.print(s);
}

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  showBoot(F("WiFi: connecting..."));
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("ctm-dash");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) delay(250);
  if (WiFi.status() != WL_CONNECTED) showBoot(F("WiFi: failed to connect"));
}

static bool syncNtp() {
  if (g_ntpReady && time(nullptr) > 1700000000L) return true;
  showBoot(F("NTP: syncing time..."));
  configTzTime(g_posixTz, "pool.ntp.org", "time.nist.gov");
  unsigned long start = millis();
  while (time(nullptr) < 1700000000L && millis() - start < 15000UL) delay(250);
  g_ntpReady = (time(nullptr) > 1700000000L);
  return g_ntpReady;
}

// --- OAuth2 Device Flow -----------------------------------------------------

static bool startDeviceFlow() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/oauth2/device_token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(15000);
  String body = String("client_id=") + CTM_CLIENT_ID +
                "&scope=" + "profile+reports+activity+manage+route_apps";
  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload.c_str())) {
      g_deviceCode    = doc["device_code"].as<const char*>();
      g_userCode      = doc["user_code"].as<const char*>();
      g_verifyUri     = doc["verification_uri"].as<const char*>();
      g_pollInterval  = doc["interval"] | 5;
      g_flowExpiresIn = doc["expires_in"] | 1500;
      g_flowStartMs   = millis();
      g_lastPollMs    = 0;
      ok = !g_deviceCode.isEmpty() && !g_userCode.isEmpty();
    }
  }
  http.end();
  return ok;
}

// Returns: 0=pending, 1=success, 2=expired/denied, 3=error
static int pollDeviceToken() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/oauth2/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(15000);
  String body = String("client_id=") + CTM_CLIENT_ID +
                "&device_code=" + g_deviceCode +
                "&grant_type=device_code";
  int code = http.POST(body);
  int result = 3;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload.c_str())) {
      const char* at = doc["access_token"].as<const char*>();
      if (at && *at) {
        String refreshToken = doc["refresh_token"].as<const char*>();
        // account_id comes as a JSON integer, convert properly
        int acctInt = doc["account_id"].as<int>();
        String acctId = acctInt > 0 ? String(acctInt) : "";
        saveTokens(at, refreshToken, acctId);
        result = 1;
      } else {
        const char* err = doc["error"].as<const char*>();
        if (err) {
          if (strcmp(err, "authorization_pending") == 0) result = 0;
          else if (strcmp(err, "slow_down") == 0) { g_pollInterval += 5; result = 0; }
          else result = 2;
        }
      }
    }
  }
  http.end();
  return result;
}

static bool refreshAccessToken() {
  if (g_refreshToken.isEmpty()) return false;
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/oauth2/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(15000);
  String body = String("client_id=") + CTM_CLIENT_ID +
                "&client_secret=" + CTM_CLIENT_SECRET +
                "&grant_type=refresh_token" +
                "&refresh_token=" + g_refreshToken;
  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload.c_str())) {
      const char* at = doc["access_token"].as<const char*>();
      if (at && *at) {
        String newRefresh = doc["refresh_token"].as<const char*>();
        if (newRefresh.isEmpty()) newRefresh = g_refreshToken;
        String acctId = doc["account_id"].as<String>();
        if (acctId.isEmpty()) acctId = g_accountId;
        saveTokens(at, newRefresh, acctId);
        ok = true;
      }
    }
  }
  http.end();
  return ok;
}

// --- API calls (Bearer auth) ------------------------------------------------

static bool fetchAccountTz() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/api/v1/accounts/" + g_accountId);
  http.addHeader("Authorization", bearerHeader());
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument filter; filter["timezone_name"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString().c_str(), DeserializationOption::Filter(filter))) {
      const char* tz = doc["timezone_name"].as<const char*>();
      if (tz && *tz) {
        const char* px = posixFor(tz);
        if (px) { strncpy(g_posixTz, px, sizeof(g_posixTz)-1); g_posixTz[sizeof(g_posixTz)-1]='\0'; ok=true; }
      }
    }
  } else if (code == 401) g_authFailed = true;
  http.end();
  return ok;
}

static void addPeakBuckets(JsonObject obj) {
  for (JsonPair p : obj) { int v = p.value().as<int>(); if (v > g_peak) g_peak = v; }
}

static bool fetchHistory() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/api/v1/accounts/" + g_accountId +
             "/calls/history.json?interval=today");
  http.addHeader("Authorization", bearerHeader());
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument filter;
    filter["in_progress_count"] = filter["inbound_calls"] = filter["outbound_calls"] = true;
    filter["chats"] = filter["missed_calls"] = filter["videos"] = true;
    filter["inbound"] = filter["outbound"] = filter["chat"] = filter["video"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString().c_str(), DeserializationOption::Filter(filter))) {
      g_active = doc["in_progress_count"].as<int>();
      g_inbound = doc["inbound_calls"].as<int>();
      g_outbound = doc["outbound_calls"].as<int>();
      g_chats = doc["chats"].as<int>();
      g_missed = doc["missed_calls"].as<int>();
      g_videos = doc["videos"].as<int>();
      g_total = g_inbound + g_outbound + g_chats + g_missed + g_videos;
      g_peak = 0;
      addPeakBuckets(doc["inbound"].as<JsonObject>());
      addPeakBuckets(doc["outbound"].as<JsonObject>());
      addPeakBuckets(doc["chat"].as<JsonObject>());
      addPeakBuckets(doc["video"].as<JsonObject>());
      ok = true;
    }
  } else if (code == 401) g_authFailed = true;
  http.end();
  time_t now = time(nullptr);
  if (now > 1700000000L) {
    struct tm* lt = localtime(&now);
    strftime(g_lastUpdated, sizeof(g_lastUpdated), "%H:%M:%S", lt);
    snprintf(g_lastDate, sizeof(g_lastDate), "%s %d/%d", DOW[lt->tm_wday], lt->tm_mon+1, lt->tm_mday);
  }
  g_status = ok ? "OK" : (code < 0 ? "NET" : String("HTTP ") + code);
  return ok;
}

static bool fetchAgents() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/api/v1/accounts/" + g_accountId +
             "/agents/history.json?bypass=cache");
  http.addHeader("Authorization", bearerHeader());
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);
  String body = F("interval=today&agent_ids=all&manager=0&manager_view_mode=recent");
  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload.c_str())) {
      g_agentsReady = g_agentsNotReady = 0;
      JsonArray agents = doc["agents"];
      g_agentsTotal = agents.isNull() ? 0 : agents.size();
      if (!agents.isNull()) {
        for (JsonObject a : agents) {
          bool accept = a["status"]["accept"].as<bool>();
          const char* v = a["status"]["value"].as<const char*>();
          if (accept && v && strcmp(v, "online") == 0) g_agentsReady++;
          else g_agentsNotReady++;
        }
      }
      ok = true;
    }
  } else if (code == 401) g_authFailed = true;
  http.end();
  return ok;
}

static bool fetchRecentCalls() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/api/v1/accounts/" + g_accountId +
             "/calls?direction=inbound&status=answered&per_page=2&sort=desc");
  http.addHeader("Authorization", bearerHeader());
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument filter;
    filter["calls"][0]["summary"] = true;
    filter["calls"][0]["direction"] = true;
    JsonDocument doc;
    if (!deserializeJson(doc, payload.c_str(), DeserializationOption::Filter(filter))) {
      g_summaryCount = 0;
      g_tickerText = "";
      JsonArray calls = doc["calls"];
      if (!calls.isNull()) {
        for (JsonObject c : calls) {
          const char* summary = c["summary"].as<const char*>();
          const char* dir = c["direction"].as<const char*>();
          if (summary && *summary && g_summaryCount < MAX_SUMMARIES) {
            String entry = "";
            if (dir) {
              if (strcmp(dir, "inbound") == 0) entry = "IN: ";
              else if (strcmp(dir, "outbound") == 0) entry = "OUT: ";
              else if (strcmp(dir, "msg_inbound") == 0) entry = "TXT: ";
              else if (strcmp(dir, "msg_outbound") == 0) entry = "TXT: ";
              else if (strcmp(dir, "chat") == 0) entry = "CHAT: ";
              else if (strcmp(dir, "form") == 0) entry = "FORM: ";
            }
            entry += String(summary);
            if (entry.length() > 120) entry = entry.substring(0, 117) + "...";
            g_summaries[g_summaryCount++] = entry;
            g_tickerText += entry + "  |  ";
          }
        }
        ok = true;
      }
    }
  } else if (code == 401) g_authFailed = true;
  http.end();
  return ok;
}

// --- Auth screen (device flow UI) -------------------------------------------

static void drawAuthScreen() {
  tft.fillScreen(COL_BG);

  // Header
  tft.fillRect(0, 0, 320, 26, COL_HEADER);
  drawCtmLogo(6, 0);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(38, 6);
  tft.print(F("Authorize"));

  // "Authorize This Device"
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(50, 38);
  tft.print(F("Authorize Device"));

  // Step 1
  tft.setTextColor(0x7BEF);
  tft.setTextSize(1);
  tft.setCursor(10, 68);
  tft.print(F("1. On your phone or PC, visit:"));

  // URL box
  tft.fillRect(10, 80, 300, 20, COL_TILE);
  tft.setTextColor(CTM_SKY_BLUE);
  tft.setTextSize(1);
  tft.setCursor(16, 86);
  // Strip https:// for display
  String url = g_verifyUri;
  url.replace("https://", "");
  tft.print(url);

  // Step 2
  tft.setTextColor(0x7BEF);
  tft.setCursor(10, 112);
  tft.print(F("2. Enter this code:"));

  // Code box (big, centered)
  tft.setTextSize(4);
  int codeW = tft.textWidth(g_userCode);
  int boxX = (320 - codeW - 24) / 2;
  tft.fillRect(boxX, 124, codeW + 24, 44, COL_TILE);
  tft.drawRect(boxX, 124, codeW + 24, 44, CTM_NEBULA_BLUE);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(boxX + 12, 134);
  tft.print(g_userCode);

  // Status
  unsigned long elapsed = (millis() - g_flowStartMs) / 1000;
  int remaining = g_flowExpiresIn - (int)elapsed;
  if (remaining < 0) remaining = 0;
  int mins = remaining / 60;
  int secs = remaining % 60;

  tft.setTextColor(0x7BEF);
  tft.setTextSize(1);
  tft.setCursor(10, 185);
  tft.print(F("Waiting for approval..."));

  tft.setTextColor(remaining < 60 ? COL_ERR : COL_OK);
  tft.setCursor(10, 205);
  tft.printf("%d:%02d remaining", mins, secs);
}

static void drawAuthMessage(const String& msg) {
  tft.fillRect(0, 180, 320, 60, COL_BG);
  tft.setTextColor(COL_ERR);
  tft.setTextSize(2);
  tft.setCursor(20, 195);
  tft.print(msg);
}

// --- Dashboard rendering (same as before) -----------------------------------

static void drawSmallTile(int x, int y, int w, int h, const char* label, int value, uint16_t accent) {
  tft.fillRect(x, y, w, h, COL_TILE);
  tft.fillRect(x, y, 3, h, accent);
  tft.setTextColor(COL_DIM);
  tft.setTextSize(1);
  tft.setCursor(x + 6, y + 3);
  tft.print(label);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(x + 6, y + 20);
  tft.print(value);
}

static void drawBigTile(int x, int y, int w, int h, const char* label, int value,
                        uint16_t accent, uint16_t valueColor) {
  tft.fillRect(x, y, w, h, COL_TILE);
  tft.fillRect(x, y, w, 4, accent);
  tft.setTextColor(COL_DIM);
  tft.setTextSize(1);
  tft.setCursor(x + 10, y + 10);
  tft.print(label);
  tft.setTextColor(valueColor);
  tft.setTextSize(5);
  tft.setCursor(x + 10, y + 28);
  tft.print(value);
}

static void render() {
  tft.fillScreen(COL_BG);

  // Header bar
  tft.fillRect(0, 0, 320, 26, COL_HEADER);
  drawCtmLogo(6, 0);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(38, 6);
  tft.print(F("CTM Activity"));
  tft.setTextSize(1);
  int dateW = tft.textWidth(g_lastDate);
  tft.setCursor(320 - dateW - 6, 9);
  tft.print(g_lastDate);

  // Status line
  tft.setTextColor(0x7BEF);
  tft.setTextSize(1);
  tft.setCursor(8, 32);
  tft.print(F("Updated "));
  tft.setTextColor(COL_TEXT);
  tft.print(g_lastUpdated);
  bool ok = (g_status == "OK");
  tft.fillCircle(200, 36, 4, ok ? COL_OK : COL_ERR);
  tft.setTextColor(ok ? COL_OK : COL_ERR);
  tft.setCursor(210, 32);
  tft.print(g_firstOk ? (ok ? "OK" : "ERR") : "--");

  tft.drawFastHLine(0, 44, 320, COL_DIM);

  // Two BIG tiles
  int bigY = 48, bigH = 72, bigW = 158;
  drawBigTile(0,      bigY, bigW, bigH, "ACTIVE CALLS", g_active,
              g_active > 0 ? COL_ACTIVE : COL_DIM, g_active > 0 ? COL_ACTIVE : COL_DIM);
  drawBigTile(bigW+4, bigY, bigW, bigH, "PEAK / MIN", g_peak,
              CTM_SKY_BLUE, COL_TEXT);

  tft.drawFastHLine(0, 124, 320, COL_DIM);

  // Agent strip
  int agentY = 128;
  tft.fillRect(0, agentY, 320, 18, CTM_GALAXY_GREY);
  tft.setTextColor(COL_DIM);
  tft.setTextSize(1);
  tft.setCursor(6, agentY + 5);
  tft.print(F("AGENTS"));
  tft.fillCircle(70, agentY + 9, 4, COL_OK);
  tft.setTextColor(COL_OK);
  tft.setCursor(80, agentY + 5);
  tft.printf("%d READY", g_agentsReady);
  tft.fillCircle(190, agentY + 9, 4, COL_ERR);
  tft.setTextColor(COL_ERR);
  tft.setCursor(200, agentY + 5);
  tft.printf("%d NOT READY", g_agentsNotReady);

  tft.drawFastHLine(0, 148, 320, COL_DIM);

  // 5 small tiles: IN | OUT | CHAT | MISSED | VIDEO
  // Shrunk from 80px to 60px to fit ticker at bottom
  int tileW = 63, tileH = 60, tileY = 152;
  drawSmallTile(0*tileW + 0, tileY, tileW, tileH, "IN",     g_inbound,  COL_IN);
  drawSmallTile(1*tileW + 1, tileY, tileW, tileH, "OUT",    g_outbound, COL_OUT);
  drawSmallTile(2*tileW + 2, tileY, tileW, tileH, "CHAT",   g_chats,    COL_CHAT);
  drawSmallTile(3*tileW + 3, tileY, tileW, tileH, "MISSED", g_missed,   COL_MISS);
  drawSmallTile(4*tileW + 4, tileY, tileW, tileH, "VIDEO",  g_videos,   COL_VID);

  // Ticker strip background
  int tickerY = 214;
  tft.fillRect(0, tickerY, 320, 26, CTM_GALAXY_GREY);
  tft.drawFastHLine(0, tickerY, 320, COL_DIM);
  g_tickerOffset = 0;  // reset scroll on each render
}

static void drawTicker() {
  if (g_tickerText.isEmpty()) return;
  int tickerY = 214;
  int tickerH = 26;

  // Clear the ticker area
  tft.fillRect(0, tickerY, 320, tickerH, CTM_GALAXY_GREY);
  tft.drawFastHLine(0, tickerY, 320, COL_DIM);

  tft.setTextColor(COL_TEXT);
  tft.setTextSize(1);
  int textW = tft.textWidth(g_tickerText);

  if (textW <= 320) {
    // Text fits on screen - draw static
    tft.setCursor(0, tickerY + 9);
    tft.print(g_tickerText);
    return;
  }

  // Scrolling text wider than screen
  // Calculate which character to start drawing from based on pixel offset
  // Walk through the string, drawing chars that are within the visible window
  int x = -g_tickerOffset;
  for (unsigned int i = 0; i < g_tickerText.length(); i++) {
    char ch = g_tickerText[i];
    int charW = tft.textWidth(String(ch));
    if (x + charW > 0 && x < 320) {
      tft.drawChar(ch, x, tickerY + 9, 1);
    }
    x += charW;
    if (x >= 320) break;
  }
  // Draw second copy starting from the right edge
  x = -g_tickerOffset + textW;
  for (unsigned int i = 0; i < g_tickerText.length() && x < 320; i++) {
    char ch = g_tickerText[i];
    int charW = tft.textWidth(String(ch));
    if (x + charW > 0) {
      tft.drawChar(ch, x, tickerY + 9, 1);
    }
    x += charW;
    if (x >= 320) break;
  }

  // Advance offset
  g_tickerOffset += 2;
  if (g_tickerOffset >= textW) g_tickerOffset = 0;
}

// --- Setup / Loop -----------------------------------------------------------

void setup() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  loadTokens();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (!g_accessToken.isEmpty()) {
      // Have a stored token - try to use it
      g_authFailed = false;
      syncNtp();
      fetchAccountTz();
      if (g_authFailed) {
        // Token expired - try refresh
        g_authFailed = false;
        if (!refreshAccessToken()) {
          clearTokens();
          g_state = STATE_DEVICE_FLOW;
          return;
        }
        fetchAccountTz();
      }
      if (!g_authFailed) {
        fetchAgents();
        fetchRecentCalls();
        if (fetchHistory()) {
          g_firstOk = true;
          g_state = STATE_DASHBOARD;
          render();
          return;
        }
      }
    }
    // No token or everything failed - start device flow
    g_state = STATE_DEVICE_FLOW;
  } else {
    showBoot(F("WiFi: down"));
  }
}

void loop() {
  unsigned long now = millis();

  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      delay(5000);
      return;
    }
  }

  switch (g_state) {

  case STATE_DEVICE_FLOW: {
    // Start or restart device flow
    if (g_deviceCode.isEmpty() || (now - g_flowStartMs) / 1000 >= g_flowExpiresIn) {
      if (!g_deviceCode.isEmpty()) {
        drawAuthMessage(F("Code expired"));
        delay(2000);
      }
      if (!startDeviceFlow()) {
        showBoot(F("Device flow failed"));
        delay(5000);
        return;
      }
      g_deviceCode = g_deviceCode;  // keep code
    }

    drawAuthScreen();

    // Poll at the specified interval
    if (now - g_lastPollMs >= (unsigned long)g_pollInterval * 1000UL) {
      g_lastPollMs = now;
      int result = pollDeviceToken();
      if (result == 1) {
        // Success! Switch to dashboard
        g_deviceCode = "";
        syncNtp();
        fetchAccountTz();
        fetchAgents();
        fetchRecentCalls();
        if (fetchHistory()) {
          g_firstOk = true;
          g_state = STATE_DASHBOARD;
          render();
        } else {
          g_state = STATE_DASHBOARD;
          render();
        }
      } else if (result == 2) {
        // Expired or denied - restart
        drawAuthMessage(F("Denied or expired"));
        delay(2000);
        g_deviceCode = "";
      }
      // result 0 = pending, keep waiting
      // result 3 = error, keep trying
    }
    delay(200);
    break;
  }

  case STATE_DASHBOARD: {
    static unsigned long lastFetch = 0;
    if (now - lastFetch >= REFRESH_MS) {
      lastFetch = now;
      g_authFailed = false;
      if (!g_ntpReady) syncNtp();
      fetchAgents();
      fetchRecentCalls();
      bool ok = fetchHistory();

      if (g_authFailed) {
        g_authFailed = false;
        if (refreshAccessToken()) {
          fetchAgents();
          fetchRecentCalls();
          ok = fetchHistory();
        } else {
          clearTokens();
          g_state = STATE_DEVICE_FLOW;
          g_deviceCode = "";
          return;
        }
      }

      if (ok) { g_firstOk = true; render(); }
      else if (g_firstOk) render();
      else showBoot(String(F("No data: ")) + g_status);
    }
    // Smooth ticker scroll - 50ms per pixel
    drawTicker();
    delay(50);
    break;
  }

  case STATE_BOOT:
  default:
    if (WiFi.status() == WL_CONNECTED) {
      if (!g_accessToken.isEmpty()) {
        g_authFailed = false;
        syncNtp();
        fetchAccountTz();
        if (g_authFailed) {
          g_authFailed = false;
          if (refreshAccessToken()) {
            fetchAccountTz();
            fetchAgents();
            fetchRecentCalls();
            if (fetchHistory()) { g_firstOk = true; g_state = STATE_DASHBOARD; render(); return; }
          } else { clearTokens(); g_state = STATE_DEVICE_FLOW; return; }
        } else {
          fetchAgents();
          fetchRecentCalls();
          if (fetchHistory()) { g_firstOk = true; g_state = STATE_DASHBOARD; render(); return; }
          g_state = STATE_DEVICE_FLOW;
        }
      } else {
        g_state = STATE_DEVICE_FLOW;
      }
    }
    delay(1000);
    break;
  }
}
