# Submersible Motor Controller — ESP8266 NodeMCU

A WiFi-enabled controller for a submersible motor pump, built on the ESP8266 NodeMCU. Supports physical button control (single press, double press, hold-to-force-off) and a mobile-friendly web dashboard with a live countdown ring, timer presets, custom duration slider, and a password-gated unlimited-run mode.

By **Developerkk** — [www.developerkk.in](http://www.developerkk.in)

---

## Features

- 📶 WiFi status LED (fast blink = connecting, solid = connected, slow blink = dropped/reconnecting)
- 🔘 Physical push-button control with single-press, double-press, and hold-to-force-off gestures
- 🌐 Web dashboard reachable at a static IP, with live status syncing every 2 seconds
- ⏱️ Two editable timer presets (default 5 min / 10 min), matched to the physical button
- 🎚️ Custom duration slider (1–120 minutes)
- ♾️ Password-protected "unlimited run" mode
- 🔌 Works offline — the motor keeps running on its own timer even if WiFi drops

---

## Hardware / Wiring

| ESP8266 Pin | GPIO | Function |
|---|---|---|
| D1 | GPIO 5 | Relay 1 — WiFi status indicator LED |
| D4 | GPIO 2 | Relay 2 — Motor (phase A) |
| D5 | GPIO 14 | Relay 3 — Motor (phase B) |
| D6 | GPIO 12 | Relay 4 — Motor (phase C) |
| D2 | GPIO 4 | TIP push-button (other leg → GND) |

- Relays 2, 3, and 4 switch together to drive the submersible motor.
- The button uses the ESP8266's internal pull-up and is active-LOW.
- Most 4-channel relay boards are active-LOW; this is already configured in the code (`RELAY_ON = LOW`, `RELAY_OFF = HIGH`). If your relay board is active-HIGH, swap these two `#define`s.

---

## Button Behavior

The TIP button is active-LOW with an internal pull-up.

| Action | Result |
|---|---|
| 1 press | Motor ON for `DEFAULT_TIME_1` (default: 5 min) |
| 2 presses within 600 ms | Motor ON for `DEFAULT_TIME_2` (default: 10 min) |
| Hold for 3 s | FORCE OFF — overrides any WiFi command and clears the timer |

While the motor is running, a button press first checks for a hold:

- **Quick press** → turns the motor OFF (toggle)
- **Hold 3 s** → FORCE OFF (same as above)

---

## Web Dashboard

Once connected, the dashboard is available at:

```
http://192.168.1.51/
```

The page includes:

- A countdown ring with `MM:SS` display
- Two preset buttons (5-min / 10-min by default) mirroring the physical button
- A custom-duration slider (10 s – 60 min, adjustable in 30 s steps)
- ON / OFF / STOP controls
- A timer-configuration panel with a scroll-drum ("wheel") picker for setting the two presets
- Status polling every 2 seconds via `/api/status` (JSON)
- Offline resilience — the motor keeps running on its own even if the browser loses connection

### API Endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Serves the web dashboard |
| `/api/status` | GET | Returns current motor/WiFi/timer state as JSON |
| `/api/on?dur=<seconds>` | GET | Turns the motor ON for `<seconds>` (0 = run indefinitely) |
| `/api/off` | GET | Turns the motor OFF |
| `/api/setTimers?t1=<sec>&t2=<sec>` | GET | Updates the two default timer presets |

Example `/api/status` response:

```json
{
  "motor": true,
  "dur": 300,
  "rem": 214,
  "wifi": true,
  "t1": 300,
  "t2": 600
}
```

---

## Setup

### 1. Requirements

- Arduino IDE (or PlatformIO) with ESP8266 board support installed
- Libraries: `ESP8266WiFi`, `ESP8266WebServer`, `Ticker` (all bundled with the ESP8266 board package)
- A NodeMCU / ESP8266 board
- A 4-channel relay module (active-LOW by default)

### 2. Configure WiFi and network

Edit the following near the top of the sketch:

```cpp
const char* WIFI_SSID     = "Wifi SSID";
const char* WIFI_PASSWORD = "WIFI Password";

IPAddress staticIP (192, 168, 1,  51);
IPAddress gateway  (192, 168, 1,   1);
IPAddress subnet   (255, 255, 255, 0);
IPAddress dns1     (  8,   8, 8,   8);
```

Make sure the static IP doesn't collide with another device on your network, and that it's outside your router's DHCP range (or reserved for this device).

### 3. Set the unlimited-run password

The "∞" button on the dashboard is gated by a password, hardcoded in the page's JavaScript:

```js
var UNLIMITED_PASSWORD = '2560';
```

Change this to your own value before deploying. Note that this is a **client-side check only** — it's a convenience safeguard against accidental taps, not a real security control, since the source is visible to anyone who views the page.

### 4. Flash and power on

Upload the sketch to your board. On boot, the WiFi LED (Relay 1) will blink fast while connecting. After ~12 seconds without a connection, the device falls back to offline mode (button control still works; the web UI won't be reachable until WiFi returns).

### 5. Open the dashboard

Navigate to `http://192.168.1.51/` (or whatever static IP you configured) from a phone or computer on the same network.

---

## Tunable Parameters

| Constant | Default | Description |
|---|---|---|
| `DBL_PRESS_WINDOW_MS` | 600 ms | Max gap between presses to register as a double-press |
| `HOLD_FORCE_OFF_MS` | 3000 ms | Hold duration required to trigger force-off |
| `DEBOUNCE_MS` | 50 ms | Button debounce window |
| `WIFI_BLINK_FAST_MS` | 200 ms | LED blink rate while connecting |
| `WIFI_BLINK_SLOW_MS` | 800 ms | LED blink rate when WiFi is dropped |
| `g_timer1` | 300 s (5 min) | Default duration for single press / preset 1 |
| `g_timer2` | 600 s (10 min) | Default duration for double press / preset 2 |

Timer presets can also be changed at runtime from the web dashboard's picker — updates are persisted via `/api/setTimers` and reflected immediately on the physical button.

---

## Notes

- The relay board polarity (`RELAY_ON` / `RELAY_OFF`) assumes active-LOW; verify against your specific relay module before wiring up a live motor.
- The `MotorWiFiState` enum is deliberately named to avoid colliding with `WiFiState`, which is already declared inside `ESP8266WiFiGeneric.h`.
- Always test relay wiring and motor control logic without the motor connected (or with a lamp as a stand-in load) before running it against real pump hardware.

---

## 📺 YouTube Channel

🎥 Learn more here: 👉 [https://www.youtube.com/@developerkk](https://www.youtube.com/@developerkk)

## 📄 License

For educational and research purposes only.

## 👨‍💻 Author

**DeveloperKK**
🌐 [https://developerkk.in](https://developerkk.in)
📧 [info@developerkk.in](mailto:info@developerkk.in)

## 💖 Support

Buy Me a Coffee ☕ 👉 [https://razorpay.me/@elevatebharat](https://razorpay.me/@elevatebharat)

## 💸 Collaboration

Open to collaboration on IoT, home automation, and embedded projects — sponsorships, feature requests, and PRs are all welcome. Reach out via email or open an issue if you'd like to work together or suggest an improvement.
