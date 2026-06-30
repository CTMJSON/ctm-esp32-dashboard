# CTM ESP32 Activity Dashboard

<img width="3484" height="2613" alt="IMG_9761" src="https://github.com/user-attachments/assets/94fbbf8d-5d07-4ada-b81c-e4d70076f91a" />


A compact, always-on activity dashboard for [CTM](https://www.calltrackingmetrics.com) that runs on an ESP32 with a built-in color TFT. It sits on a desk and refreshes every 60 seconds, showing live call, chat, and agent activity for the current day.

## Hardware

- **Board:** ESP32-2432S028R ("Cheap Yellow Display" / CYD)
  - ESP32-D0WD-V3 (dual-core, Wi-Fi + BT), 4 MB flash
  - Built-in 2.8" 320x240 ILI9341 SPI TFT (resistive touch via XPT2046, unused here)
  - USB-powered and programmed; no external wiring required
- Any ESP32 + ILI9341 setup works with minor pin changes in `User_Setup.h`.

## What it shows

```
┌──────────────────────────────────────────┐
│ CTM Activity            Tuesday 6/30     │  header (Nebula Blue)
│ Updated 14:23:01        ● OK             │  status dot + last refresh
├──────────────────────────────────────────┤
│ ACTIVE CALLS    PEAK / MIN               │
│        0                2                │  two big tiles
├──────────────────────────────────────────┤
│ AGENTS  ● 1 READY   ● 3 NOT READY        │  agent strip (Galaxy Grey)
├──────────────────────────────────────────┤
│▌IN  ▌OUT ▌CHAT▌MISS▌VID                 │
│▌ 0  ▌ 1  ▌ 2  ▌ 0  ▌ 0                  │  five metric tiles
└──────────────────────────────────────────┘
```

- **Active Calls** — `in_progress_count` from the history endpoint (lime when > 0)
- **Peak / Min** — highest per-minute activity across all directions today
- **Agents** — ready = `accept: true` AND `value: "online"`; everything else = not ready
- **IN / OUT / CHAT / MISSED / VIDEO** — today's totals per direction

Colors follow the CTM brand palette: Space Navy background, Nebula Blue header, Dark Matter Blue tiles, Sky Blue and Supernova Lime accents.

## API endpoints used

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/v1/accounts/{id}` | GET | Read account timezone (for local clock display) |
| `/api/v1/accounts/{id}/calls/history.json?interval=today` | GET | Today's call/chat/video totals + active count + peak |
| `/api/v1/accounts/{id}/agents/history.json?bypass=cache` | POST | Agent ready/not-ready counts for today |

Authentication is Basic Auth with a CTM API access key + secret (base64-encoded).

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
#define CTM_ACCOUNT_ID "your_account_id"
#define CTM_BASIC_AUTH "base64_of_access_key:secret"
```

`CTM_BASIC_AUTH` is the base64 encoding of `access_key:secret` (find these in CTM under Account Settings → API access). It is the same value used in the `Authorization: Basic ...` header.

`secrets.h` is git-ignored — never commit real credentials.

### 4. Board settings

- **Board:** ESP32 Dev Module
- **Upload Speed:** 460800 (lower than 921600 for stability on CH340/CH9102F clones)
- **Port:** the `cu.usbserial-*` device that appears when you plug in

### 5. Upload

Open `ctm_dashboard/ctm_dashboard.ino`, set the port, click Upload. On first boot it connects Wi-Fi, syncs NTP, fetches the account timezone, then renders the dashboard.

## Notes

- HTTPS certificate validation is skipped (`WiFiClientSecure::setInsecure`). Fine for a LAN dashboard; use a CA bundle if you need it.
- NTP is used only for the on-screen clock and day-of-week; the API calls rely on the server's `interval=today` so data is correct even before NTP syncs.
- The agent status endpoint requires POST with form-encoded body (`interval=today&agent_ids=all`) — a plain GET returns only agents active in the last hour.
- The ESP32 has ~280 KB of free RAM; the ~20 KB agent response is parsed without a JSON filter (filtering array nodes is not supported by ArduinoJson's input filter).

## Project layout

```
ctm-esp32-dashboard/
├── README.md
├── .gitignore
├── secrets.h.template          # placeholder - copy to ctm_dashboard/secrets.h
└── ctm_dashboard/
    ├── ctm_dashboard.ino        # the sketch
    ├── secrets.h                # git-ignored, your real creds
    └── secrets.h.template       # placeholder copy
```
