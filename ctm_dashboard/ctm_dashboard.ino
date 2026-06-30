/*
  CTM Activity Dashboard  -  ESP32-2432S028R (Cheap Yellow Display)
  ---------------------------------------------------------------
  Board: ESP32-D0WD-V3 + built-in 2.8" 320x240 ILI9341 SPI TFT.
  No external wiring required - display is hard-wired on the board.
  Display config lives in TFT_eSPI/User_Setup.h (CYD pins, TFT_RST=12).

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
#include <time.h>
#include <stdlib.h>
#include "secrets.h"
#include "ctm_logo.h"

#define REFRESH_MS    60000UL
#define API_HOST      "api.calltrackingmetrics.com"

TFT_eSPI tft = TFT_eSPI();

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
#define CTM_SKY_BLUE      0x05FE  // #01bdf6  primary
#define CTM_NEBULA_BLUE   0x04B9  // #0796ca  primary
#define CTM_DARK_MATTER   0x0AF1  // #0e5e8c  primary
#define CTM_SPACE_NAVY    0x1149  // #16294f  primary (darkest)
#define CTM_LIME          0xD6C0  // #d6da01  secondary accent
#define CTM_GALAXY_GREY   0x3186  // #333333  neutral

// Dashboard color mapping
#define COL_HEADER   CTM_NEBULA_BLUE
#define COL_FOOTER   CTM_DARK_MATTER
#define COL_BG       CTM_SPACE_NAVY
#define COL_OK       CTM_LIME
#define COL_ERR      TFT_RED
#define COL_TEXT     TFT_WHITE
#define COL_DIM      TFT_WHITE
#define COL_TILE     CTM_DARK_MATTER
#define COL_ACTIVE   CTM_LIME
#define COL_ACTIVEBG CTM_DARK_MATTER
#define COL_IN       CTM_SKY_BLUE
#define COL_OUT      CTM_NEBULA_BLUE
#define COL_CHAT     CTM_LIME
#define COL_MISS     0xFD20  // soft amber (semantic warning, non-brand)
#define COL_VID      CTM_NEBULA_BLUE

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

static char g_posixTz[48] = "EST5EDT,M3.2.0,M11.1.0";
static bool g_ntpReady = false;
static bool g_firstOk = false;

static int g_active = 0, g_inbound = 0, g_outbound = 0;
static int g_chats = 0, g_missed = 0, g_videos = 0, g_total = 0;
static int g_peak = 0;
static int g_agentsReady = 0, g_agentsNotReady = 0, g_agentsTotal = 0;

static char g_lastUpdated[9] = "--:--:--";
static char g_lastDate[20]   = "------";
static String g_status = "boot";

static const char* const DOW[] = {
  "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

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

static String authHeader() { return String("Basic ") + CTM_BASIC_AUTH; }

static bool fetchAccountTz() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/api/v1/accounts/" + CTM_ACCOUNT_ID);
  http.addHeader("Authorization", authHeader());
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
  }
  http.end();
  return ok;
}

static void addPeakBuckets(JsonObject obj) {
  for (JsonPair p : obj) { int v = p.value().as<int>(); if (v > g_peak) g_peak = v; }
}

static bool fetchHistory() {
  WiFiClientSecure s; s.setInsecure();
  HTTPClient http;
  http.begin(s, String("https://") + API_HOST + "/api/v1/accounts/" + CTM_ACCOUNT_ID +
             "/calls/history.json?interval=today");
  http.addHeader("Authorization", authHeader());
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
  }
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
  http.begin(s, String("https://") + API_HOST + "/api/v1/accounts/" + CTM_ACCOUNT_ID +
             "/agents/history.json?bypass=cache");
  http.addHeader("Authorization", authHeader());
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);
  String body = F("interval=today&agent_ids=all&manager=0&manager_view_mode=recent");
  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString().c_str())) {
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
  }
  http.end();
  return ok;
}

static void drawSmallTile(int x, int y, int w, int h, const char* label, int value, uint16_t accent) {
  tft.fillRect(x, y, w, h, COL_TILE);
  tft.fillRect(x, y, 3, h, accent);
  tft.setTextColor(COL_DIM);
  tft.setTextSize(1);
  tft.setCursor(x + 6, y + 4);
  tft.print(label);
  tft.setTextColor(COL_TEXT);
  tft.setTextSize(3);
  tft.setCursor(x + 6, y + 30);
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

  // Two BIG tiles: ACTIVE CALLS | PEAK / MIN
  int bigY = 48, bigH = 72, bigW = 158;
  drawBigTile(0,      bigY, bigW, bigH, "ACTIVE CALLS", g_active,
              g_active > 0 ? COL_ACTIVE : COL_DIM, g_active > 0 ? COL_ACTIVE : COL_DIM);
  drawBigTile(bigW+4, bigY, bigW, bigH, "PEAK / MIN", g_peak,
              CTM_SKY_BLUE, COL_TEXT);

  tft.drawFastHLine(0, 124, 320, COL_DIM);

  // Agent status strip
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
  int tileW = 63, tileH = 80, tileY = 152;
  drawSmallTile(0*tileW + 0, tileY, tileW, tileH, "IN",     g_inbound,  COL_IN);
  drawSmallTile(1*tileW + 1, tileY, tileW, tileH, "OUT",    g_outbound, COL_OUT);
  drawSmallTile(2*tileW + 2, tileY, tileW, tileH, "CHAT",   g_chats,    COL_CHAT);
  drawSmallTile(3*tileW + 3, tileY, tileW, tileH, "MISSED", g_missed,   COL_MISS);
  drawSmallTile(4*tileW + 4, tileY, tileW, tileH, "VIDEO",  g_videos,   COL_VID);
}

void setup() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    fetchAccountTz();
    syncNtp();
    fetchAgents();
    if (fetchHistory()) { g_firstOk = true; render(); }
    else showBoot(String(F("No data: ")) + g_status);
  }
}

void loop() {
  static unsigned long lastFetch = 0;
  unsigned long now = millis();
  if (now - lastFetch >= REFRESH_MS) {
    lastFetch = now;
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      if (!g_ntpReady) syncNtp();
      fetchAgents();
      bool ok = fetchHistory();
      if (ok) { g_firstOk = true; render(); }
      else if (g_firstOk) render();
      else showBoot(String(F("No data: ")) + g_status);
    } else if (g_firstOk) render();
    else showBoot(F("WiFi: down"));
  }
  delay(1000);
}
