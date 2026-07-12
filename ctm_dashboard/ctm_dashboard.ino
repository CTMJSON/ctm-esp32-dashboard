/*
  CTM Activity Dashboard  -  ESP32-32E 4.0\" ST7796S 480x320
  ---------------------------------------------------------------
  Board: ESP32-D0WD-V3 + built-in 4.0" 480x320 ST7796S SPI TFT.
  Display config lives in TFT_eSPI/User_Setup.h (ESP32-32E pinout).

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

// --- 4.0" 480x320 landscape layout -----------------------------------------
#define SCREEN_W      480
#define SCREEN_H      320
#define HEADER_H      30
#define BIG_TILE_Y    54
#define BIG_TILE_H    90
#define BIG_TILE_W    238
#define AGENT_Y       148
#define AGENT_H       42
#define SMALL_TILE_Y  194
#define SMALL_TILE_H  60
#define SMALL_TILE_W  94
#define TICKER_Y      258
#define TICKER_H      62
// per_page=4 yields ~30KB response, well within the 277KB free heap
#define RECENT_CALLS_PAGE_SIZE 4

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// Draw the CTM logomark with transparency (skip 0x0000 pixels).
// pushImage batches the whole 26x25 sprite into a single SPI transfer
// instead of one drawPixel() (and SPI transaction) per pixel.
static void drawCtmLogo(int x, int y) {
  tft.pushImage(x, y, CTM_LOGO_W, CTM_LOGO_H, ctm_logo, (uint16_t)0x0000);
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
  tft.setCursor(10, 30);
  tft.print(F("CTM Dashboard"));
  tft.setTextSize(1);
  tft.setTextColor(0x7BEF);
  tft.setCursor(10, 60);
  tft.print(F("ESP32-32E 4.0\" ST7796"));
  tft.setTextColor(COL_TEXT);
  tft.setCursor(10, 90);
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
        // account_id is often omitted on refresh responses; keep the existing
        // one in that case instead of risking a stringified "null" landing
        // in NVS (as<String>() on a missing/null key is not reliably empty).
        String acctId = g_accountId;
        if (!doc["account_id"].isNull()) {
          int acctInt = doc["account_id"].as<int>();
          if (acctInt > 0) acctId = String(acctInt);
        }
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
    // 274KB total but agents array is only ~42KB (series array is 186KB)
    // Read first 50KB into a heap buffer - enough for agents, skip series
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) { http.end(); return false; }
    stream->setTimeout(15000);

    size_t bufSize = 50000;
    char* buf = (char*)malloc(bufSize);
    if (!buf) { http.end(); return false; }

    size_t total = 0;
    unsigned long lastData = millis();
    while (millis() - lastData < 5000UL && total < bufSize - 1) {
      while (stream->available() && total < bufSize - 1) {
        int n = stream->readBytes(buf + total, bufSize - 1 - total);
        if (n > 0) { total += n; lastData = millis(); }
        else break;
      }
      delay(5);
    }
    buf[total] = '\0';

    // Truncate at "series" and close the JSON object manually
    char* seriesPos = strstr(buf, "\"series\"");
    if (seriesPos) {
      char* p = seriesPos - 1;
      while (p > buf && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r')) p--;
      *(p + 1) = '}';
      *(p + 2) = '\0';
      total = p + 2 - buf;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf, total);
    free(buf);

    if (!err) {
      g_agentsReady = g_agentsNotReady = 0;
      JsonArray agents = doc["agents"];
      g_agentsTotal = agents.isNull() ? 0 : agents.size();
      if (!agents.isNull()) {
        for (JsonObject a : agents) {
          bool accept = a["status"]["accept"].as<bool>();
          const char* v = a["status"]["value"].as<const char*>();
          bool online = (v && strcmp(v, "online") == 0);
          if (accept && online) g_agentsReady++; else g_agentsNotReady++;
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
             "/calls?direction=inbound&status=answered&has_transcription=1&per_page=" +
             String(RECENT_CALLS_PAGE_SIZE) + "&sort=desc");
  http.addHeader("Authorization", bearerHeader());
  http.addHeader("Accept", "application/json");
  http.setTimeout(20000);
  int code = http.GET();
  bool ok = false;
  String newTicker = "";
  int newCount = 0;
  if (code == 200) {
    // WiFiClientSecure's read() is non-blocking: it returns whatever's
    // currently buffered and 0/-1 when nothing has arrived *yet* (not
    // necessarily EOF - TLS records land in discrete chunks). Parsing
    // straight off the stream with ArduinoJson doesn't retry on that, so it
    // reports IncompleteInput on anything larger than what fits in a single
    // TLS record. Buffer the full (known-size, from Content-Length) response
    // first with the same idle-timeout poll loop fetchAgents() already uses
    // below, then parse the complete buffer.
    int contentLen = http.getSize();
    size_t bufSize = contentLen > 0 ? (size_t)contentLen + 1 : 90000;
    char* buf = (char*)malloc(bufSize);
    if (!buf) { http.end(); return false; }

    WiFiClient* stream = http.getStreamPtr();
    size_t total = 0;
    unsigned long lastData = millis();
    while (millis() - lastData < 8000UL && total < bufSize - 1) {
      while (stream->available() && total < bufSize - 1) {
        int n = stream->readBytes(buf + total, bufSize - 1 - total);
        if (n > 0) { total += n; lastData = millis(); }
        else break;
      }
      delay(5);
    }
    buf[total] = '\0';

    JsonDocument filter;
    filter["calls"][0]["summary"] = true;
    filter["calls"][0]["direction"] = true;
    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, buf, total, DeserializationOption::Filter(filter));
    free(buf);

    if (!jerr) {
      JsonArray calls = doc["calls"];
      if (!calls.isNull()) {
        for (JsonObject c : calls) {
          const char* summary = c["summary"].as<const char*>();
          const char* dir = c["direction"].as<const char*>();
          if (summary && *summary && newCount < MAX_SUMMARIES) {
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
            newTicker += entry + "  |  ";
            newCount++;
          }
        }
        ok = true;
      }
    }
  } else if (code == 401) g_authFailed = true;
  http.end();
  // Only replace ticker if we got new data; keep old text on failure
  if (ok && newTicker.length() > 0) {
    g_tickerText = newTicker;
    g_summaryCount = newCount;
    g_tickerOffset = 0;
  }
  return ok;
}

// --- Auth screen (device flow UI) -------------------------------------------

static void drawAuthScreen() {
  // Batch all the fills/text/pixels below into a single SPI transaction
  // instead of one open+close per draw call.
  tft.startWrite();
  tft.fillScreen(COL_BG);

  // Header
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HEADER);
  drawCtmLogo(6, (HEADER_H - CTM_LOGO_H) / 2);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(38, 8);
  tft.print(F("Authorize"));

  // "Authorize This Device"
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(160, 40);
  tft.print(F("Authorize Device"));

  // Step 1
  tft.setTextColor(0x7BEF);
  tft.setTextSize(1);
  tft.setCursor(20, 68);
  tft.print(F("1. On your phone or PC, visit:"));

  // URL box
  tft.fillRect(20, 80, SCREEN_W - 40, 22, COL_TILE);
  tft.setTextColor(CTM_SKY_BLUE);
  tft.setTextSize(1);
  tft.setCursor(26, 86);
  String url = g_verifyUri;
  url.replace("https://", "");
  tft.print(url);

  // Step 2
  tft.setTextColor(0x7BEF);
  tft.setCursor(20, 114);
  tft.print(F("2. Enter this code:"));

  // Code box (big, centered)
  tft.setTextSize(4);
  int codeW = tft.textWidth(g_userCode);
  int boxX = (SCREEN_W - codeW - 24) / 2;
  tft.fillRect(boxX, 128, codeW + 24, 52, COL_TILE);
  tft.drawRect(boxX, 128, codeW + 24, 52, CTM_NEBULA_BLUE);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(boxX + 12, 138);
  tft.print(g_userCode);

  // Status
  unsigned long elapsed = (millis() - g_flowStartMs) / 1000;
  int remaining = g_flowExpiresIn - (int)elapsed;
  if (remaining < 0) remaining = 0;
  int mins = remaining / 60;
  int secs = remaining % 60;

  tft.setTextColor(0x7BEF);
  tft.setTextSize(1);
  tft.setCursor(20, 198);
  tft.print(F("Waiting for approval..."));

  tft.setTextColor(remaining < 60 ? COL_ERR : COL_OK);
  tft.setCursor(20, 218);
  tft.printf("%d:%02d remaining", mins, secs);
  tft.endWrite();
}

static void drawAuthMessage(const String& msg) {
  tft.fillRect(0, 190, SCREEN_W, 60, COL_BG);
  tft.setTextColor(COL_ERR);
  tft.setTextSize(2);
  tft.setCursor(80, 208);
  tft.print(msg);
}

// --- Dashboard rendering (same as before) -----------------------------------

static void drawSmallTile(int x, int y, int w, int h, const char* label, int value, uint16_t accent) {
  tft.fillRect(x, y, w, h, COL_TILE);
  tft.fillRect(x, y, 3, h, accent);
  tft.setTextColor(COL_DIM);
  tft.setTextSize(1);
  tft.setCursor(x + 6, y + 6);
  tft.print(label);
  // Center the number roughly in the tile height
  int valY = y + h / 2 - 6;
  if (valY < y + 20) valY = y + 20;
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(3);
  tft.setCursor(x + 6, valY);
  tft.print(value);
}

static void drawBigTile(int x, int y, int w, int h, const char* label, int value,
                        uint16_t accent, uint16_t valueColor) {
  tft.fillRect(x, y, w, h, COL_TILE);
  tft.fillRect(x, y, w, 4, accent);
  tft.setTextColor(COL_DIM);
  tft.setTextSize(1);
  tft.setCursor(x + 10, y + 12);
  tft.print(label);
  tft.setTextColor(valueColor);
  tft.setTextSize(6);
  tft.setCursor(x + 10, y + 40);
  tft.print(value);
}

static void render() {
  tft.startWrite();
  tft.fillScreen(COL_BG);

  // Header bar
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HEADER);
  drawCtmLogo(6, (HEADER_H - CTM_LOGO_H) / 2);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(2);
  tft.setCursor(38, 8);
  tft.print(F("CTM Activity"));
  tft.setTextSize(1);
  int dateW = tft.textWidth(g_lastDate);
  tft.setCursor(SCREEN_W - dateW - 6, 10);
  tft.print(g_lastDate);

  // Status line
  tft.setTextColor(0x7BEF);
  tft.setTextSize(1);
  tft.setCursor(8, 36);
  tft.print(F("Updated "));
  tft.setTextColor(COL_TEXT);
  tft.print(g_lastUpdated);
  bool ok = (g_status == "OK");
  tft.fillCircle(SCREEN_W/2 - 20, 40, 4, ok ? COL_OK : COL_ERR);
  tft.setTextColor(ok ? COL_OK : COL_ERR);
  tft.setCursor(SCREEN_W/2 - 10, 36);
  tft.print(g_firstOk ? (ok ? "OK" : "ERR") : "--");

  tft.drawFastHLine(0, 48, SCREEN_W, COL_DIM);

  // Two BIG tiles side by side
  drawBigTile(0,          BIG_TILE_Y, BIG_TILE_W, BIG_TILE_H,
              "ACTIVE CALLS", g_active,
              g_active > 0 ? COL_ACTIVE : COL_DIM,
              g_active > 0 ? COL_ACTIVE : COL_DIM);
  drawBigTile(BIG_TILE_W + 4, BIG_TILE_Y, BIG_TILE_W, BIG_TILE_H,
              "PEAK / MIN", g_peak, CTM_SKY_BLUE, COL_TEXT);

  tft.drawFastHLine(0, BIG_TILE_Y + BIG_TILE_H, SCREEN_W, COL_DIM);

  // Agent strip
  tft.fillRect(0, AGENT_Y, SCREEN_W, AGENT_H, CTM_GALAXY_GREY);
  tft.setTextSize(2);
  tft.setTextColor(COL_DIM);
  tft.setCursor(10, AGENT_Y + 13);
  tft.print(F("AGENTS"));
  tft.fillCircle(140, AGENT_Y + AGENT_H/2, 6, COL_OK);
  tft.setTextColor(COL_OK);
  tft.setCursor(152, AGENT_Y + 13);
  tft.printf("%d READY", g_agentsReady);
  tft.fillCircle(300, AGENT_Y + AGENT_H/2, 6, COL_ERR);
  tft.setTextColor(COL_ERR);
  tft.setCursor(312, AGENT_Y + 13);
  tft.printf("%d NOT READY", g_agentsNotReady);

  tft.drawFastHLine(0, AGENT_Y + AGENT_H, SCREEN_W, COL_DIM);

  // 5 small tiles: IN | OUT | CHAT | MISSED | VIDEO
  int tileGap = (SCREEN_W - 5 * SMALL_TILE_W) / 4;
  for (int i = 0; i < 5; i++) {
    const char* label;
    int value; uint16_t accent;
    switch (i) {
      case 0: label = "IN";     value = g_inbound;  accent = COL_IN;   break;
      case 1: label = "OUT";    value = g_outbound; accent = COL_OUT;  break;
      case 2: label = "CHAT";   value = g_chats;    accent = COL_CHAT; break;
      case 3: label = "MISSED"; value = g_missed;   accent = COL_MISS; break;
      case 4: label = "VIDEO";  value = g_videos;   accent = COL_VID;  break;
    }
    int tx = i * (SMALL_TILE_W + tileGap);
    drawSmallTile(tx, SMALL_TILE_Y, SMALL_TILE_W, SMALL_TILE_H, label, value, accent);
  }

  // Ticker strip background
  tft.fillRect(0, TICKER_Y, SCREEN_W, TICKER_H, CTM_GALAXY_GREY);
  tft.drawFastHLine(0, TICKER_Y, SCREEN_W, COL_DIM);

  tft.endWrite();
}

// GLCD font (font 1) is a fixed 6px-per-char monospace at text size 1, so
// per-character widths are constant - no need to query tft.textWidth() (and
// allocate a throwaway String) for every glyph on every 50ms ticker frame.
#define TICKER_CHAR_W 12  // GLCD font at text size 2

static void drawTicker() {
  if (g_tickerText.isEmpty()) return;
  int len = g_tickerText.length();
  int textW = len * TICKER_CHAR_W;

  tft.startWrite();

  // Use background color on text to clear old chars, no full fillRect needed
  tft.setTextColor(COL_TEXT, CTM_GALAXY_GREY);
  tft.setTextSize(2);

  int cursorY = TICKER_Y + TICKER_H / 2 - 8;
  if (cursorY < TICKER_Y + 16) cursorY = TICKER_Y + 16;

  if (textW <= SCREEN_W) {
    tft.setCursor(0, cursorY);
    tft.print(g_tickerText);
    tft.endWrite();
    return;
  }

  // Scrolling text wider than screen
  int x = -g_tickerOffset;
  for (int i = 0; i < len && x < SCREEN_W; i++) {
    if (x + TICKER_CHAR_W > 0) tft.drawChar(g_tickerText[i], x, cursorY, 1);
    x += TICKER_CHAR_W;
  }
  x = -g_tickerOffset + textW;
  for (int i = 0; i < len && x < SCREEN_W; i++) {
    if (x + TICKER_CHAR_W > 0) tft.drawChar(g_tickerText[i], x, cursorY, 1);
    x += TICKER_CHAR_W;
  }

  tft.endWrite();

  g_tickerOffset += 4;
  if (g_tickerOffset >= textW) g_tickerOffset = 0;
}

// --- Setup / Loop -----------------------------------------------------------

void setup() {
  tft.init();
  tft.setRotation(1);   // 480x320 landscape
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
    }

    drawAuthScreen();

    // Poll at the specified interval
    // (device code is retained until success, expiry, or denial)
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
  default: {
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
}
