# CTM Activity Dashboard 

<img width="1195" height="896" alt="IMG_9766" src="https://github.com/user-attachments/assets/6e7b0434-c33c-4302-99fe-f1dca86de0f0" />


A little desk gadget that shows your [CTM](https://www.calltrackingmetrics.com) account's live call and chat activity — active calls, today's totals, agent status, and a scrolling ticker of recent call summaries — on a cheap ESP32 board with a built-in color screen. It refreshes every 60 seconds and just sits there being useful (and kind of fun to glance at).

**Heads up:** this is a personal hobby project, not an official CTM product. I built it for my own desk and figured other CTM users might enjoy it too. It's shared as-is, no support SLA, but I'm happy to help — see [Questions & support](#questions--support) below.

— Jason Smith (jason.smith@ctm.com)

## What it looks like on the screen

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

- **Active Calls** — calls in progress right now (lights up lime when > 0)
- **Peak / Min** — the busiest minute so far today, across calls/chats/video combined
- **Agents** — how many are ready to take a call vs. not (accepting calls *and* online)
- **IN / OUT / CHAT / MISSED / VIDEO** — today's totals by type
- **Ticker** — a scrolling feed of your most recent answered call summaries (the AI-generated ones), so you get a feel for what's coming in without opening the app

Colors follow the CTM brand palette: Space Navy background, Nebula Blue header, Dark Matter Blue tiles, Sky Blue and Supernova Lime accents.

## What you'll need

| Item | Notes |
|---|---|---|
| **ESP32-2432S028R** ("Cheap Yellow Display" / CYD) | ~$10-15. 2.8" 320x240 ILI9341. Search "ESP32 2.8 inch CYD". |
| **ESP32-32E 4.0"** (ST7796S) | ~$15-25. 4.0" 480x320 ST7796S. Search "ESP32-32E 4.0 inch ST7796". |
| **USB-C or Micro-USB cable** (check which your board uses) | For programming and power. It can stay plugged into any USB power source afterward. |
| **A computer with the Arduino IDE** | Free, works on Mac/Windows/Linux. This is a one-time setup step. |
| **A CTM account** with permission to create OAuth apps | You'll register a small OAuth client (takes 2 minutes) — no API keys get hardcoded into the firmware. |
| **Wi-Fi** | The board needs a 2.4GHz network (ESP32s don't do 5GHz). |

Both board variants are built-in display ESP32s — nothing to wire up.

## Setting it up

This takes about 15-20 minutes the first time, mostly waiting on downloads. The code auto-detects the display variant at build time via `User_Setup.h`, so you just need the right pinout.

### 1. Install the Arduino IDE and libraries

1. Download the [Arduino IDE](https://www.arduino.cc/en/software) (2.x) if you don't have it.
2. Add ESP32 board support: **Arduino IDE → Settings → Additional boards manager URLs**, add `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`, then **Tools → Board → Boards Manager**, search "esp32", install the Espressif package.
3. Install two libraries via **Sketch → Include Library → Manage Libraries**:
   - **TFT_eSPI** (by Bodmer)
   - **ArduinoJson** v7+ (by Benoit Blanchon)

(Adafruit GFX/ILI9341 aren't used here, no need to install them.)

### 2. Configure the display driver

TFT_eSPI needs to know your board's pin layout. Find the library's `User_Setup.h` (usually under your Arduino libraries folder → `TFT_eSPI/User_Setup.h`) and replace its contents with the matching configuration below.

**Option A: 2.8" CYD (ESP32-2432S028R, ILI9341, 320x240)**

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

The one setting that really matters for this board is `TFT_RST = 12` (some CYD guides leave this at `-1`, which leaves the screen blank).

**Option B: 4.0" ESP32-32E (ST7796S, 480x320 landscape)**

```c
#define USER_SETUP_INFO "ESP32-32E-ST7796"
#define ST7796_DRIVER
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH
#define TFT_WIDTH  320
#define TFT_HEIGHT 480
#define SPI_FREQUENCY      40000000
#define SPI_READ_FREQUENCY  20000000
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_GFXFF
#define SMOOTH_FONT
```

Key differences from the CYD: `TFT_RST = -1` (connected to ESP32 RST), `TFT_BL = 27` (not 21 — wrong pin gives a black screen), and `ST7796_DRIVER` instead of `ILI9341_2_DRIVER`. The sketch uses rotation 1 to render at 480x320 landscape.

Other ESP32 display boards work too — swap in your board's driver and pins.

### 3. Register an OAuth app in CTM

The dashboard authenticates using CTM's OAuth2 **device flow** — the same pattern smart TVs and streaming boxes use to log into a service ("visit this URL and enter this code"). Nothing sensitive ever gets typed on the device itself, and no long-lived API key gets baked into the firmware.

1. In CTM, go to your account's [API/OAuth app settings](https://app.calltrackingmetrics.com/oauth_apps/new) and create a new OAuth application.
2. Enable **Device** flow.
3. Check all scopes (`profile`, `reports`, `activity`, `manage`, `route_apps`).
4. Save it and copy the **Client ID** and **Client Secret** — you'll need those next.

### 4. Add your Wi-Fi and OAuth credentials

Copy `secrets.h.template` to `ctm_dashboard/secrets.h` and fill in your own values:

```c
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
#define CTM_CLIENT_ID "your_oauth_client_id"
#define CTM_CLIENT_SECRET "your_oauth_client_secret"
```

`secrets.h` is git-ignored, so your real credentials never get committed if you fork or push changes.

### 5. Flash it

1. Plug the board in via USB.
2. Open `ctm_dashboard/ctm_dashboard.ino` in the Arduino IDE.
3. **Tools → Board:** `ESP32 Dev Module`
4. **Tools → Upload Speed:** `460800` (the common CYD USB chips — CH340/CH9102F — can be flaky at the default 921600)
5. **Tools → Port:** pick the `cu.usbserial-*` / `COM*` port that shows up when the board is plugged in
6. Click **Upload**

### 6. Connect your CTM account

On first boot, the screen walks you through it:

1. Connects to your Wi-Fi
2. Shows a short code and a URL (`app.calltrackingmetrics.com/accesscode`)
3. Grab your phone or another computer, visit that URL, and enter the code
4. Once you approve it, the board finishes syncing and the dashboard appears

That's it — tokens are stored on the device (in flash, not on a server) and refresh themselves automatically, so it'll go straight to the dashboard on every reboot after that. You won't need to log in again unless you unplug it for a very long time or revoke the OAuth app.

## Questions & support

This is a for-fun side project, so there's no formal support channel — but if you're a fellow CTM user setting one of these up and get stuck, or just want to share what you built, email me at **jason.smith@ctm.com**. Happy to help troubleshoot or hear ideas for what else would be good to show on it.

## Troubleshooting

- **Screen stays black after flashing** — double check `TFT_RST` is `12`, not `-1`, in your `User_Setup.h`. This is the #1 gotcha on CYD boards.
- **Upload fails / times out** — try a lower upload speed (`115200`), a different USB cable (some are charge-only), or hold the board's `BOOT` button while upload starts.
- **Stuck on "WiFi: connecting..."** — the ESP32 only supports 2.4GHz networks; make sure that band is enabled on your router, and double-check `WIFI_SSID`/`WIFI_PASS` in `secrets.h`.
- **Device code screen never goes away** — you have ~25 minutes to enter the code before it expires and generates a new one; make sure you're visiting the exact URL shown and entering the code without extra spaces.
- **Dashboard shows "ERR" or stale data** — this usually clears itself on the next 60-second refresh. If it persists, the OAuth token may have been revoked on the CTM side; unplug/replug the device to force a fresh device-flow login.

## How it works, for the curious

### Authentication

OAuth2 device flow, no hardcoded API keys. Tokens are stored in the ESP32's NVS (non-volatile flash storage) and persist across reboots; access tokens refresh automatically in the background.

### API endpoints used

| Endpoint | Method | Purpose |
|---|---|---|
| `/oauth2/device_token` | POST | Start device flow, get user code |
| `/oauth2/token` | POST | Poll for access token / refresh token |
| `/api/v1/accounts/{id}` | GET | Read account timezone (for local clock display) |
| `/api/v1/accounts/{id}/calls/history.json?interval=today` | GET | Today's call/chat/video totals + active count + peak |
| `/api/v1/accounts/{id}/agents/history.json?bypass=cache` | POST | Agent ready/not-ready counts for today |
| `/api/v1/accounts/{id}/calls?direction=inbound&status=answered&has_transcription=1&per_page=8&sort=desc` | GET | Recent call transcriptions for the ticker |

All requests use Bearer token auth (`Authorization: Bearer {access_token}`).

### Implementation notes

- HTTPS certificate validation is skipped (`WiFiClientSecure::setInsecure`). That's fine for a desk gadget on a home/office LAN; swap in a CA bundle if you need stricter validation.
- NTP is used only for the on-screen clock and day-of-week — the API calls rely on the server's `interval=today`, so the numbers are correct even before NTP has synced.
- The agent status endpoint needs a POST with a form-encoded body (`interval=today&agent_ids=all`); a plain GET only returns agents active in the last hour.
- The recent-calls response is buffered into a heap buffer sized to the response's `Content-Length` and freed right after parsing, rather than held for the life of the request — that's what makes `per_page=8` (up from an earlier `per_page=2`) affordable on the ESP32's limited RAM.
- The agents/history endpoint returns a much larger payload (~270KB, mostly a `series` array we don't need); the sketch reads only the first 50KB into a heap buffer, truncates at `"series"`, and parses just the agents array (~40KB) out of that.
- The ESP32 has roughly 280KB of free RAM to work with; screen redraws are batched into single SPI transactions for smoother, faster updates.
- Token version is tracked in NVS; bumping the version flag in the code automatically clears old tokens on the next boot (useful after changing OAuth scopes).

## Project layout

```
ctm-esp32-dashboard/
├── README.md
├── .gitignore
├── secrets.h.template              # placeholder - copy to ctm_dashboard/secrets.h
├── png_to_rgb565.py                # PNG to RGB565 C header converter (for the logo)
├── ttf_to_vlw.py                   # TTF to TFT_eSPI .vlw font converter (for future custom fonts)
└── ctm_dashboard/
    ├── ctm_dashboard.ino           # the sketch
    ├── ctm_logo.h                  # CTM logomark as PROGMEM RGB565 array
    ├── secrets.h                   # git-ignored, your real creds
    └── secrets.h.template          # placeholder copy
```
