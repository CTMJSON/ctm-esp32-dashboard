# CTM ESP32 Activity Dashboard

<img width="3484" height="2613" alt="IMG_9761" src="https://github.com/user-attachments/assets/94fbbf8d-5d07-4ada-b81c-e4d70076f91a" />


A compact, always-on activity dashboard for [CallTrackingMetrics](https://www.calltrackingmetrics.com) that runs on an ESP32 with a built-in color TFT. It sits on a desk and refreshes every 60 seconds, showing live call, chat, agent activity, and recent call summaries for the current day.

## Hardware

- **Board:** ESP32-2432S028R ("Cheap Yellow Display" / CYD)
  - ESP32-D0WD-V3 (dual-core, Wi-Fi + BT), 4 MB flash
  - Built-in 2.8" 320x240 ILI9341 SPI TFT (resistive touch via XPT2046, unused here)
  - USB-powered and programmed; no external wiring required
- Any ESP32 + ILI9341 setup works with minor pin changes in `User_Setup.h`.

## What it shows

```
┌──────────────────────────────────────────┐
│ ▲ CTM Activity          Tuesday 6/30     │  header (Nebula Blue + logo)
│ Updated 14:23:01        ● OK             │  status dot + last refresh
├──────────────────────────────────────────┤
│ ACTIVE CALLS    PEAK / MIN               │
│        0                2                │  two big tiles
├──────────────────────────────────────────┤
│ AGENTS  ● 1 READY   ● 3 NOT READY        │  agent strip (Galaxy Grey)
├──────────────────────────────────────────┤
│▌IN  ▌OUT ▌CHAT▌MISS▌VID                 │
│▌ 0  ▌ 1  ▌ 2  ▌ 0  ▌ 0                  │  five metric tiles
├──────────────────────────────────────────┤
│ IN: Allison called Peter about...        │  scrolling call summaries
└──────────────────────────────────────────┘
```

- **Active Calls** — `in_progress_count` from the history endpoint (lime when > 0)
- **Peak / Min** — highest per-minute activity across all directions today
- **Agents** — ready = `accept: true` AND `value: "online"`; everything else = not ready
- **IN / OUT / CHAT / MISSED / VIDEO** — today's totals per direction
- **Ticker** — recent inbound answered call summaries (AI-generated), scrolling along the bottom

Colors follow the CTM brand palette: Space Navy background, Nebula Blue header, Dark Matter Blue tiles, Sky Blue and Supernova Lime accents.

## Authentication: OAuth2 Device Flow

The dashboard uses CTM's OAuth2 device flow — no hardcoded API keys. On first boot (or after token expiry), the TFT displays a user code and verification URL. The user visits the URL, enters the code, and authorizes the device. Tokens are stored in NVS (persistent across reboots) and refreshed automatically.

All five CTM OAuth scopes are requested: `profile`, `reports`, `activity`, `manage`, `route_apps`.

## API endpoints used

| Endpoint | Method | Purpose |
|---|---|---|
| `/oauth2/device_token` | POST | Start device flow, get user code |
| `/oauth2/token` | POST | Poll for access token / refresh token |
| `/api/v1/accounts/{id}` | GET | Read account timezone (for local clock display) |
| `/api/v1/accounts/{id}/calls/history.json?interval=today` | GET | Today's call/chat/video totals + active count + peak |
| `/api/v1/accounts/{id}/agents/history.json?bypass=cache` | POST | Agent ready/not-ready counts for today |
| `/api/v1/accounts/{id}/calls?direction=inbound&status=answered&per_page=2&sort=desc` | GET | Recent call summaries for ticker |

Authentication is Bearer token (`Authorization: Bearer {access_token}`).

## Setup

### 1. Arduino IDE libraries

Install via **Sketch → Include Library → Manage Libraries**:

- **TFT_eSPI** (by Bodmer)
- **ArduinoJson** v7+ (by Benoit Blanchon)

Adafruit GFX/ILI9341 are not used.

### 2. Configure TFT_eSPI for the CYD

Replace `~/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h` with the following (this is the CYD pinout — the critical part is `TFT_RST = 12`):

```c
#define USER_SETUP_INFO "CYD-2432S028R"
#define ILI9341_2_DRIVER
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  12
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
#define SPI_FREQUENCY  55000000
#define SPI_READ_FREQUENCY  20000000
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_GFXFF
#define SMOOTH_FONT
```

### 3. Create your secrets file

Copy `secrets.h.template` to `ctm_dashboard/secrets.h` and fill in:

```c
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
#define CTM_CLIENT_ID "your_oauth_client_id"
#define CTM_CLIENT_SECRET "your_oauth_client_secret"
```

Register an OAuth2 app in CTM with **Device only** flow enabled and all scopes checked. Use the provided client ID and secret.

`secrets.h` is git-ignored — never commit real credentials.

### 4. Board settings

- **Board:** ESP32 Dev Module
- **Upload Speed:** 460800 (lower than 921600 for stability on CH340/CH9102F clones)
- **Port:** the `cu.usbserial-*` device that appears when you plug in

### 5. Upload

Open `ctm_dashboard/ctm_dashboard.ino`, set the port, click Upload. On first boot:
1. Connects Wi-Fi
2. Starts OAuth2 device flow — TFT shows a user code
3. Visit `app.calltrackingmetrics.com/accesscode` on your phone/PC, enter the code
4. Device polls until authorized, then stores tokens in NVS
5. Syncs NTP, fetches account timezone, renders the dashboard

Subsequent boots load tokens from NVS and go straight to the dashboard.

## Notes

- HTTPS certificate validation is skipped (`WiFiClientSecure::setInsecure`). Fine for a LAN dashboard; use a CA bundle if you need it.
- NTP is used only for the on-screen clock and day-of-week; the API calls rely on the server's `interval=today` so data is correct even before NTP syncs.
- The agent status endpoint requires POST with form-encoded body (`interval=today&agent_ids=all`) — a plain GET returns only agents active in the last hour.
- Call summaries are limited to `per_page=2` because the ESP32's HTTP buffer can't handle the full 220KB payload from larger page sizes.
- The ESP32 has ~280 KB of free RAM; the ~20 KB agent response is parsed without a JSON filter (ArduinoJson's filter doesn't work on array nodes with object patterns).
- Token version is tracked in NVS; bumping the version flag in code automatically clears old tokens on boot (useful when changing OAuth scopes).

## Project layout

```
ctm-esp32-dashboard/
├── README.md
├── .gitignore
├── secrets.h.template              # placeholder - copy to ctm_dashboard/secrets.h
├── png_to_rgb565.py                # PNG to RGB565 C header converter (for logo)
├── ttf_to_vlw.py                   # TTF to TFT_eSPI .vlw font converter
└── ctm_dashboard/
    ├── ctm_dashboard.ino           # the sketch
    ├── ctm_logo.h                  # CTM logomark as PROGMEM RGB565 array
    ├── secrets.h                   # git-ignored, your real creds
    └── secrets.h.template          # placeholder copy
```
