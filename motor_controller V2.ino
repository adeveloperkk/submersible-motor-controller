/*
=======================================================================
  SUBMERSIBLE MOTOR CONTROLLER — ESP8266 NodeMCU / Wemos D1 Mini
=======================================================================

  WIRING:
  ┌──────────────────────────────────────────────────────────────┐
  │  ESP8266 Pin  │  What                                         │
  ├───────────────┼──────────────────────────────────────────────┤
  │  D1 (GPIO 5)  │  Relay 1  — WiFi status indicator LED        │
  │  D4 (GPIO 2)  │  Relay 2  — Motor (phase A)                  │
  │  D5 (GPIO 14) │  Relay 3  — Motor (phase B)                  │
  │  D6 (GPIO 12) │  Relay 4  — Motor (phase C)                  │
  │  D2 (GPIO 4)  │  TIP push-button  (other leg → GND)          │
  └───────────────┴──────────────────────────────────────────────┘

  BEHAVIOUR:
  • R1  → Blinks fast while connecting.  Solid ON = WiFi OK.
          Blinks slow = WiFi lost (auto-reconnect running).
  • R2+R3+R4 switch together (submersible motor).

  BUTTON LOGIC  (TIP button, active-LOW with internal pull-up)
  ─────────────────────────────────────────────────────────────
  1 press  → motor ON for DEFAULT_TIME_1  (default 2 min)
  2 presses within 600 ms → motor ON for DEFAULT_TIME_2  (default 5 min)
  Hold 3 s → FORCE OFF  (overrides WiFi command, clears timer)

  While motor is ON a button press first checks for hold:
    • Quick press  → turns motor OFF (toggle)
    • Hold 3 s    → FORCE OFF (same as above)

  WEB UI  http://192.168.1.51/
  ─────────────────────────────────────────────────────────────
  • Countdown ring with MM:SS display
  • Two preset buttons (2-min / 5-min) matching physical button
  • Custom-duration slider  (10 s – 60 min, step 30 s)
  • ON / OFF / STOP buttons
  • Timer-config panel with scroll-drum picker (like screenshot)
  • Status syncs every 2 s via /api/status JSON
  • Works offline — motor keeps running even if WiFi drops

=======================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Ticker.h>

// ── WiFi credentials ─────────────────────────────────────────────────
const char* WIFI_SSID     = "WIFI NAME";
const char* WIFI_PASSWORD = "WIFI PASSWORD";

// ── Static IP ─────────────────────────────────────────────────────────
IPAddress staticIP (192, 168, 1,  51);
IPAddress gateway  (192, 168, 1,   1);
IPAddress subnet   (255, 255, 255, 0);
IPAddress dns1     (  8,   8, 8,   8);

// ── Pin map ───────────────────────────────────────────────────────────
#define PIN_RELAY_WIFI  5   // D1
#define PIN_RELAY_M1    2   // D4
#define PIN_RELAY_M2   14   // D5
#define PIN_RELAY_M3   12   // D6
#define PIN_BUTTON      4   // D2

// Most 4-ch relay boards are active-LOW
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// ── Timing tunables ───────────────────────────────────────────────────
#define DBL_PRESS_WINDOW_MS  600UL   // window for second press
#define HOLD_FORCE_OFF_MS   3000UL   // hold duration for force-off
#define DEBOUNCE_MS           50UL
#define WIFI_BLINK_FAST_MS   200UL   // connecting
#define WIFI_BLINK_SLOW_MS   800UL   // dropped

// ── Default timer durations (seconds) ────────────────────────────────
uint32_t g_timer1 = 120;   // single press  = 2 min
uint32_t g_timer2 = 300;   // double press  = 5 min

// ── Motor state ───────────────────────────────────────────────────────
bool     g_motorOn       = false;
uint32_t g_motorStartMs  = 0;
uint32_t g_motorDurSec   = 0;   // 0 = indefinite (web toggle without timer)
bool     g_forceOff      = false;

// ── WiFi state ────────────────────────────────────────────────────────
// NOTE: "WiFiState" is already declared in ESP8266WiFiGeneric.h — use a
// project-specific name to avoid the redefinition compile error.
enum MotorWiFiState { WF_CONNECTING, WF_CONNECTED, WF_DROPPED };
MotorWiFiState g_wifiState = WF_CONNECTING;
Ticker    g_wifiTicker;
bool      g_wifiLed = false;

// ── Web server ────────────────────────────────────────────────────────
ESP8266WebServer g_server(80);

// ── Button FSM ────────────────────────────────────────────────────────
enum BtnFSM { B_IDLE, B_FIRST_DOWN, B_FIRST_UP, B_HOLD_WAIT };
BtnFSM   g_btnFSM      = B_IDLE;
uint32_t g_btnTimer    = 0;
bool     g_btnLastRaw  = HIGH;
uint32_t g_debounceT   = 0;
bool     g_btnStable   = HIGH;   // debounced state

// ── Helpers ───────────────────────────────────────────────────────────
void motorsSet(bool on) {
  uint8_t s = on ? RELAY_ON : RELAY_OFF;
  digitalWrite(PIN_RELAY_M1, s);
  digitalWrite(PIN_RELAY_M2, s);
  digitalWrite(PIN_RELAY_M3, s);
  g_motorOn = on;
  if (!on) {
    g_motorDurSec  = 0;
    g_motorStartMs = 0;
  }
  Serial.printf("[MOTOR] %s\n", on ? "ON" : "OFF");
}

void motorStart(uint32_t durSec) {
  motorsSet(true);
  g_motorDurSec  = durSec;
  g_motorStartMs = millis();
  Serial.printf("[MOTOR] started, duration %u s\n", durSec);
}

uint32_t motorRemaining() {
  if (!g_motorOn || g_motorDurSec == 0) return 0;
  uint32_t elapsed = (millis() - g_motorStartMs) / 1000UL;
  if (elapsed >= g_motorDurSec) return 0;
  return g_motorDurSec - elapsed;
}

// ── WiFi LED blink callback (Ticker) ──────────────────────────────────
void wifiLedTick() {
  g_wifiLed = !g_wifiLed;
  digitalWrite(PIN_RELAY_WIFI, g_wifiLed ? RELAY_ON : RELAY_OFF);
}

void setWifiState(MotorWiFiState ws) {
  if (ws == g_wifiState) return;
  g_wifiState = ws;
  g_wifiTicker.detach();
  switch (ws) {
    case WF_CONNECTING:
      g_wifiTicker.attach_ms(WIFI_BLINK_FAST_MS, wifiLedTick);
      break;
    case WF_CONNECTED:
      digitalWrite(PIN_RELAY_WIFI, RELAY_ON);
      Serial.printf("[WIFI] Connected  IP:%s\n", WiFi.localIP().toString().c_str());
      break;
    case WF_DROPPED:
      g_wifiTicker.attach_ms(WIFI_BLINK_SLOW_MS, wifiLedTick);
      Serial.println("[WIFI] Dropped — reconnecting...");
      break;
  }
}

// ── Button debounce + FSM ─────────────────────────────────────────────
void buttonTick() {
  bool raw = digitalRead(PIN_BUTTON);   // LOW = pressed

  // Debounce
  if (raw != g_btnLastRaw) {
    g_debounceT  = millis();
    g_btnLastRaw = raw;
  }
  if (millis() - g_debounceT < DEBOUNCE_MS) return;

  bool pressed = (raw == LOW);   // stable, true = physically pressed

  switch (g_btnFSM) {

    case B_IDLE:
      if (pressed) {
        g_btnTimer = millis();
        g_btnFSM   = B_FIRST_DOWN;
      }
      break;

    case B_FIRST_DOWN:
      // still held — check for 3-second force-off
      if (pressed) {
        if (millis() - g_btnTimer >= HOLD_FORCE_OFF_MS) {
          Serial.println("[BTN] Hold 3s → FORCE OFF");
          motorsSet(false);
          g_forceOff = true;
          g_btnFSM   = B_HOLD_WAIT;
        }
      } else {
        // released before 3 s
        g_btnFSM   = B_FIRST_UP;
        g_btnTimer = millis();   // start double-press window
      }
      break;

    case B_FIRST_UP:
      if (pressed) {
        // Second press within window → double-press
        if (millis() - g_btnTimer <= DBL_PRESS_WINDOW_MS) {
          Serial.println("[BTN] Double press → timer2");
          if (g_motorOn) { motorsSet(false); }
          else           { motorStart(g_timer2); }
          g_btnFSM = B_HOLD_WAIT;
        }
      } else {
        // Timed out → single press confirmed
        if (millis() - g_btnTimer > DBL_PRESS_WINDOW_MS) {
          if (g_motorOn) {
            Serial.println("[BTN] Single press → motor OFF");
            motorsSet(false);
          } else {
            Serial.println("[BTN] Single press → timer1");
            motorStart(g_timer1);
          }
          g_btnFSM = B_IDLE;
        }
      }
      break;

    case B_HOLD_WAIT:
      // wait for release after hold or double-press
      if (!pressed) g_btnFSM = B_IDLE;
      break;
  }
}

// ══════════════════════════════════════════════════════════════════════
//  WEB UI  (stored in PROGMEM to save RAM)
// ══════════════════════════════════════════════════════════════════════

// The full HTML / CSS / JS page is returned by /
// It polls /api/status every 2 s.

static const char HTML_PAGE[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Motor Control</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect x='9' y='13' width='14' height='13' rx='3' fill='%231a1a1a'/><rect x='13' y='4' width='6' height='11' rx='2' fill='%231a1a1a'/><path d='M6 29 Q10 27 16 29 Q22 31 26 29' stroke='%231a1a1a' stroke-width='2.5' fill='none' stroke-linecap='round'/><circle cx='16' cy='19' r='4' fill='white'/><polygon points='15,17 14,20 16,19 15,22 17,18 15.5,19 17,17' fill='%231a1a1a'/></svg>">
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
html,body{height:100%;background:#f2f0eb;font-family:'Helvetica Neue',Helvetica,Arial,sans-serif;color:#1a1a1a;overflow-x:hidden}
body{display:flex;justify-content:center;align-items:flex-start;min-height:100vh}
.phone{width:100%;max-width:390px;min-height:100vh;background:#f2f0eb;padding:0 0 32px;position:relative}

/* ── top bar ── */
.topbar{display:flex;justify-content:space-between;align-items:center;padding:52px 24px 0}
.topbar-title{font-size:13px;letter-spacing:.12em;color:#999;text-transform:uppercase;font-weight:500}
.wifi-badge{display:flex;align-items:center;gap:6px;font-size:12px;color:#999}
.wifi-dot{width:7px;height:7px;border-radius:50%;background:#ccc;transition:background .4s}
.wifi-dot.on{background:#27ae60;box-shadow:0 0 6px #27ae60}
.wifi-dot.blink{animation:blink .8s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}

/* ── countdown ring ── */
.ring-wrap{display:flex;flex-direction:column;align-items:center;padding:32px 0 8px;position:relative}
.ring-svg{width:190px;height:190px;transform:rotate(-90deg)}
.ring-bg{fill:none;stroke:#e0ddd6;stroke-width:10}
.ring-arc{fill:none;stroke:#1a1a1a;stroke-width:10;stroke-linecap:round;
  stroke-dasharray:534.07;stroke-dashoffset:534.07;transition:stroke-dashoffset .9s linear}
.ring-inner{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);text-align:center;pointer-events:none}
.ring-time{font-size:44px;font-weight:300;letter-spacing:-.04em;line-height:1;font-variant-numeric:tabular-nums}
.ring-sub{font-size:11px;letter-spacing:.12em;color:#999;text-transform:uppercase;margin-top:5px}
.status-chip{margin-top:12px;padding:5px 16px;border-radius:20px;font-size:12px;letter-spacing:.08em;
  text-transform:uppercase;font-weight:500;background:#e0ddd6;color:#888;transition:all .35s}
.status-chip.running{background:#1a1a1a;color:#f2f0eb}

/* ── preset cards ── */
.presets{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:24px 20px 0}
.pcard{background:#fff;border-radius:18px;padding:18px 16px 14px;cursor:pointer;
  border:2px solid transparent;transition:border .2s,transform .12s;user-select:none;position:relative}
.pcard:active{transform:scale(.97)}
.pcard.active{border-color:#1a1a1a}
.pcard-val{font-size:24px;font-weight:300;letter-spacing:-.04em;font-variant-numeric:tabular-nums}
.pcard-label{font-size:11px;color:#999;letter-spacing:.06em;text-transform:uppercase;margin-top:3px}
.pcard-hint{font-size:10px;color:#ccc;margin-top:8px}
.pcard-icon{font-size:18px;margin-bottom:8px;opacity:.35}
.edit-btn{position:absolute;top:10px;right:10px;width:22px;height:22px;border-radius:50%;
  background:#f2f0eb;border:none;cursor:pointer;display:flex;align-items:center;justify-content:center;
  font-size:11px;color:#aaa}

/* ── main toggle ── */
.main-btn-wrap{padding:20px 20px 0}
.main-btn{width:100%;padding:17px;border-radius:16px;font-size:15px;letter-spacing:.06em;
  text-transform:uppercase;font-weight:500;cursor:pointer;transition:all .2s;border:2px solid #1a1a1a;
  background:#1a1a1a;color:#f2f0eb}
.main-btn:active{transform:scale(.98)}
.main-btn.off{background:transparent;color:#e74c3c;border-color:#e74c3c}

/* ── slider section ── */
.slider-section{padding:20px 20px 0}
.slider-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}
.slider-label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#999}
.slider-val{font-size:14px;font-weight:500;font-variant-numeric:tabular-nums}
input[type=range]{width:100%;-webkit-appearance:none;height:4px;border-radius:2px;
  background:linear-gradient(to right,#1a1a1a 0%,#1a1a1a var(--pct,50%),#e0ddd6 var(--pct,50%),#e0ddd6 100%);outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;
  border-radius:50%;background:#1a1a1a;cursor:pointer;border:3px solid #f2f0eb;box-shadow:0 0 0 1px #1a1a1a}
.start-custom{width:100%;margin-top:10px;padding:14px;border-radius:14px;font-size:13px;
  letter-spacing:.06em;text-transform:uppercase;font-weight:500;cursor:pointer;
  background:#e0ddd6;color:#555;border:none;transition:background .2s}
.start-custom:active{background:#ccc}

/* ── picker modal ── */
.picker-overlay{position:fixed;inset:0;background:rgba(0,0,0,.4);z-index:100;
  display:flex;align-items:flex-end;justify-content:center;opacity:0;pointer-events:none;transition:opacity .25s}
.picker-overlay.show{opacity:1;pointer-events:all}
.picker-sheet{width:100%;max-width:390px;background:#fff;border-radius:22px 22px 0 0;padding:0 0 40px;
  transform:translateY(100%);transition:transform .3s cubic-bezier(.4,0,.2,1)}
.picker-overlay.show .picker-sheet{transform:translateY(0)}
.picker-header{display:flex;justify-content:space-between;align-items:center;padding:20px 24px 0}
.picker-title{font-size:14px;font-weight:500;letter-spacing:.06em;text-transform:uppercase;color:#1a1a1a}
.picker-done{padding:8px 18px;background:#1a1a1a;color:#f2f0eb;border-radius:20px;
  font-size:13px;font-weight:500;border:none;cursor:pointer;letter-spacing:.04em}
.picker-which{display:flex;justify-content:center;gap:10px;padding:16px 0 4px}
.pick-tab{padding:6px 20px;border-radius:20px;font-size:12px;cursor:pointer;
  letter-spacing:.06em;text-transform:uppercase;font-weight:500;border:1.5px solid #ddd;color:#888;transition:all .2s}
.pick-tab.active{border-color:#1a1a1a;color:#1a1a1a;background:#f2f0eb}
.drums-wrap{display:flex;justify-content:center;align-items:center;gap:10px;padding:8px 0 0;position:relative}
.drum-col{display:flex;flex-direction:column;align-items:center;gap:3px}
.drum-lbl{font-size:10px;color:#bbb;letter-spacing:.1em;text-transform:uppercase;margin-bottom:6px}
.drum{width:90px;height:168px;overflow-y:scroll;scroll-snap-type:y mandatory;-ms-overflow-style:none;scrollbar-width:none;
  border-radius:14px;background:#f7f5f0;position:relative}
.drum::-webkit-scrollbar{display:none}
.drum-item{height:56px;display:flex;align-items:center;justify-content:center;
  font-size:28px;font-weight:300;color:#ccc;scroll-snap-align:center;cursor:pointer;
  transition:color .15s;font-variant-numeric:tabular-nums;letter-spacing:-.02em}
.drum-item.sel{color:#1a1a1a;font-weight:400}
.drum-pad{height:56px}
.drum-ring{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);
  width:90px;height:58px;border-top:2px solid #1a1a1a;border-bottom:2px solid #1a1a1a;
  pointer-events:none;border-radius:2px}
.drum-sep{font-size:32px;font-weight:200;color:#1a1a1a;padding-bottom:4px;align-self:center}

/* ── footer ── */
.footer{padding:24px 20px 0;text-align:center}
.footer-ip{font-size:11px;color:#bbb;letter-spacing:.08em;margin-bottom:10px}
.footer-powered{font-size:10px;color:#ccc;letter-spacing:.1em;text-transform:uppercase;margin-bottom:8px}
.footer-logo{height:32px;width:auto;opacity:.75;transition:opacity .2s;display:inline-block;vertical-align:middle}
.footer-logo:hover{opacity:1}

/* ── offline overlay ── */
.offline-overlay{position:fixed;inset:0;background:#f2f0eb;z-index:200;
  display:none;flex-direction:column;align-items:center;justify-content:center;
  padding:40px 24px;text-align:center}
.offline-overlay.show{display:flex}
.offline-anim-wrap{position:relative;width:180px;height:160px;margin-bottom:28px}
.offline-svg{width:180px;height:160px;overflow:visible}
.pump-body-rect{animation:pump-float 3s ease-in-out infinite}
@keyframes pump-float{0%,100%{transform:translateY(0)}50%{transform:translateY(-6px)}}
.wifi-arc{stroke-dasharray:80;animation:arc-fade 2s ease-in-out infinite}
.arc1{animation-delay:0s;stroke:#e74c3c}
.arc2{animation-delay:.3s;stroke:#e74c3c}
.arc3{animation-delay:.6s;stroke:#e74c3c}
@keyframes arc-fade{0%,100%{opacity:.15}50%{opacity:1}}
.pulse-ring{animation:pulse-ring 2s ease-out infinite}
@keyframes pulse-ring{0%{r:18;opacity:.8}100%{r:36;opacity:0}}
.offline-title{font-size:20px;font-weight:500;letter-spacing:-.02em;color:#1a1a1a;margin-bottom:8px}
.offline-sub{font-size:13px;color:#999;line-height:1.6;margin-bottom:24px}
.offline-count{font-size:12px;color:#bbb;letter-spacing:.08em;margin-bottom:20px;font-variant-numeric:tabular-nums}
.offline-retry-btn{padding:14px 36px;background:#1a1a1a;color:#f2f0eb;border:none;
  border-radius:14px;font-size:13px;font-weight:500;letter-spacing:.06em;text-transform:uppercase;
  cursor:pointer;transition:transform .15s}
.offline-retry-btn:active{transform:scale(.97)}
</style>
</head>
<body>
<div class="phone">

  <!-- top bar -->
  <div class="topbar">
    <span class="topbar-title">Motor</span>
    <div class="wifi-badge">
      <div class="wifi-dot" id="wDot"></div>
      <span id="wLabel">—</span>
    </div>
  </div>

  <!-- countdown ring -->
  <div class="ring-wrap">
    <div style="position:relative;width:190px;height:190px">
      <svg class="ring-svg" viewBox="0 0 190 190">
        <circle class="ring-bg" cx="95" cy="95" r="85"/>
        <circle class="ring-arc" cx="95" cy="95" r="85" id="ringArc"/>
      </svg>
      <div class="ring-inner">
        <div class="ring-time" id="ringTime">--:--</div>
        <div class="ring-sub" id="ringSubLabel">standby</div>
      </div>
    </div>
    <div class="status-chip" id="statusChip">Motor Off</div>
  </div>

  <!-- preset cards -->
  <div class="presets">
    <div class="pcard" id="pc1" onclick="presetStart(1)">
      <button class="edit-btn" onclick="event.stopPropagation();openPicker(1)" title="Edit">✎</button>
      <div class="pcard-icon">○</div>
      <div class="pcard-val" id="pc1val">2:00</div>
      <div class="pcard-label">Default · 1×</div>
      <div class="pcard-hint">Single press</div>
    </div>
    <div class="pcard" id="pc2" onclick="presetStart(2)">
      <button class="edit-btn" onclick="event.stopPropagation();openPicker(2)" title="Edit">✎</button>
      <div class="pcard-icon">◎</div>
      <div class="pcard-val" id="pc2val">5:00</div>
      <div class="pcard-label">Default · 2×</div>
      <div class="pcard-hint">Double press</div>
    </div>
  </div>

  <!-- main toggle -->
  <div class="main-btn-wrap">
    <button class="main-btn" id="mainBtn" onclick="toggleMotor()">Start Motor</button>
  </div>

  <!-- custom duration slider -->
  <div class="slider-section">
    <div class="slider-header">
      <span class="slider-label">Custom Timer</span>
      <span class="slider-val" id="sliderVal">10:00</span>
    </div>
    <input type="range" id="durSlider" min="1" max="120" step="1" value="10"
      oninput="sliderMove(this)">
    <button class="start-custom" onclick="startCustom()">▶ Start with this timer</button>
  </div>

  <!-- footer -->
  <div class="footer">
    <div class="footer-ip">192.168.1.51</div>
    <div class="footer-powered">Powered by</div>
    <a href="https://elevatebharat.com" target="_blank" rel="noopener noreferrer">
      <img src="https://elevatebharat.com/files/files/static/img/landing/intro/ElevateBharat_Logo.png"
           class="footer-logo" alt="ElevateBharat" onerror="this.style.display='none'">
    </a>
  </div>
</div>

<!-- ── offline overlay ─────────────────────────────── -->
<div class="offline-overlay" id="offlineOverlay">
  <div class="offline-anim-wrap">
    <svg class="offline-svg" viewBox="0 0 180 160">
      <!-- pulse ring behind pump -->
      <circle cx="90" cy="105" r="18" fill="none" stroke="#e74c3c" stroke-width="2" class="pulse-ring"/>
      <!-- pump body -->
      <g class="pump-body-rect">
        <rect x="65" y="82" width="50" height="48" rx="10" fill="#1a1a1a"/>
        <rect x="76" y="38" width="28" height="50" rx="6" fill="#1a1a1a"/>
        <!-- coupling -->
        <rect x="72" y="82" width="36" height="8" rx="4" fill="#333"/>
        <!-- window circle -->
        <circle cx="90" cy="108" r="14" fill="#f2f0eb"/>
        <!-- X inside window -->
        <line x1="83" y1="101" x2="97" y2="115" stroke="#e74c3c" stroke-width="3" stroke-linecap="round"/>
        <line x1="97" y1="101" x2="83" y2="115" stroke="#e74c3c" stroke-width="3" stroke-linecap="round"/>
        <!-- intake holes -->
        <rect x="58" y="102" width="8" height="3" rx="1.5" fill="#333"/>
        <rect x="58" y="109" width="8" height="3" rx="1.5" fill="#333"/>
        <rect x="114" y="102" width="8" height="3" rx="1.5" fill="#333"/>
        <rect x="114" y="109" width="8" height="3" rx="1.5" fill="#333"/>
      </g>
      <!-- WiFi arcs (cut/broken) -->
      <path class="wifi-arc arc3" d="M52 54 Q90 14 128 54" stroke="#e74c3c" stroke-width="3" fill="none" stroke-linecap="round"/>
      <path class="wifi-arc arc2" d="M62 62 Q90 32 118 62" stroke="#e74c3c" stroke-width="3" fill="none" stroke-linecap="round"/>
      <path class="wifi-arc arc1" d="M74 70 Q90 52 106 70" stroke="#e74c3c" stroke-width="3" fill="none" stroke-linecap="round"/>
      <!-- water waves at bottom -->
      <path d="M30 148 Q52 142 74 148 Q96 154 118 148 Q140 142 160 148" stroke="#bbb" stroke-width="2" fill="none" stroke-linecap="round"/>
      <path d="M20 156 Q50 150 80 156 Q110 162 140 156 Q155 152 162 156" stroke="#ddd" stroke-width="1.5" fill="none" stroke-linecap="round"/>
    </svg>
  </div>
  <div class="offline-title">Device Unreachable</div>
  <div class="offline-sub">Motor Controller is not<br>connected to WiFi network</div>
  <div class="offline-count" id="offlineCount">Retrying in 10s…</div>
  <button class="offline-retry-btn" onclick="retryNow()">↺ Retry Now</button>
</div>

<!-- ── picker modal ────────────────────────────── -->
<div class="picker-overlay" id="pickerOverlay" onclick="overlayClick(event)">
  <div class="picker-sheet">
    <div class="picker-header">
      <span class="picker-title" id="pickerTitle">Set Timer</span>
      <button class="picker-done" onclick="pickerDone()">Save</button>
    </div>
    <div class="picker-which">
      <div class="pick-tab active" id="tab1" onclick="switchTab(1)">1× Press</div>
      <div class="pick-tab"        id="tab2" onclick="switchTab(2)">2× Press</div>
    </div>
    <div class="drums-wrap">
      <div class="drum-col">
        <div class="drum-lbl">Min</div>
        <div style="position:relative">
          <div class="drum" id="drumMin" onscroll="drumScroll('drumMin')"></div>
          <div class="drum-ring"></div>
        </div>
      </div>
      <div class="drum-sep">:</div>
      <div class="drum-col">
        <div class="drum-lbl">Sec</div>
        <div style="position:relative">
          <div class="drum" id="drumSec" onscroll="drumScroll('drumSec')"></div>
          <div class="drum-ring"></div>
        </div>
      </div>
    </div>
  </div>
</div>

<script>
// ── state ───────────────────────────────────────────────────────────
var t1 = 120, t2 = 300;          // timer defaults in seconds
var motorOn = false, motorDur = 0, remaining = 0;
var wifiOk = false;
var pollTimer = null, countTimer = null;
var localRemaining = 0, localDur = 0;
const C = 2 * Math.PI * 85;      // 534.07  (circumference)
var failCount = 0, retryCount = 0, retryTimer = null;

// ── init ────────────────────────────────────────────────────────────
window.onload = function() {
  buildDrums();
  updatePresetLabels();
  initSlider();
  poll();
  pollTimer = setInterval(poll, 2000);
};

// ── API poll ─────────────────────────────────────────────────────────
function poll() {
  fetch('/api/status').then(r=>r.json()).then(function(d) {
    failCount = 0;
    hideOffline();
    motorOn   = d.motor;
    motorDur  = d.dur;
    remaining = d.rem;
    wifiOk    = d.wifi;
    t1 = d.t1; t2 = d.t2;
    updatePresetLabels();
    applyStatus();
    if (motorOn && motorDur > 0) { startLocalCount(remaining, motorDur); }
    else if (!motorOn)           { clearLocalCount(); }
  }).catch(function() {
    failCount++;
    setWifiDot(false);
    if (failCount >= 3) { showOffline(); }
  });
}

// ── apply state to UI ────────────────────────────────────────────────
function applyStatus() {
  setWifiDot(wifiOk);
  var chip = document.getElementById('statusChip');
  var btn  = document.getElementById('mainBtn');
  var arc  = document.getElementById('ringArc');
  var rt   = document.getElementById('ringTime');
  var rs   = document.getElementById('ringSubLabel');

  if (motorOn) {
    chip.className = 'status-chip running';
    chip.textContent = 'Running';
    btn.className  = 'main-btn off';
    btn.textContent = 'Stop Motor';
    if (motorDur > 0) {
      rs.textContent = 'remaining';
    } else {
      rt.textContent = '∞';
      rs.textContent = 'running';
      arc.style.strokeDashoffset = C * 0.1; // almost full ring
    }
  } else {
    chip.className = 'status-chip';
    chip.textContent = 'Motor Off';
    btn.className  = 'main-btn';
    btn.textContent = 'Start Motor';
    rt.textContent = '--:--';
    rs.textContent = 'standby';
    arc.style.strokeDashoffset = C;
  }

  // active preset highlight
  document.getElementById('pc1').className = 'pcard' + (motorOn && motorDur===t1 ? ' active':'');
  document.getElementById('pc2').className = 'pcard' + (motorOn && motorDur===t2 ? ' active':'');
}

// ── local countdown (interpolates between polls) ─────────────────────
function startLocalCount(rem, dur) {
  clearLocalCount();
  localRemaining = rem;
  localDur = dur;
  tickCount();
  countTimer = setInterval(tickCount, 1000);
}
function clearLocalCount() {
  if (countTimer) clearInterval(countTimer);
  countTimer = null;
}
function tickCount() {
  if (localRemaining <= 0) { clearLocalCount(); poll(); return; }
  var m = Math.floor(localRemaining / 60);
  var s = localRemaining % 60;
  document.getElementById('ringTime').textContent =
    String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');
  document.getElementById('ringSubLabel').textContent = 'remaining';
  var frac = localDur > 0 ? localRemaining / localDur : 0;
  document.getElementById('ringArc').style.strokeDashoffset = C * (1 - frac);
  localRemaining--;
}

// ── WiFi dot ─────────────────────────────────────────────────────────
function setWifiDot(ok) {
  var d = document.getElementById('wDot');
  var l = document.getElementById('wLabel');
  d.className = 'wifi-dot' + (ok ? ' on' : ' blink');
  l.textContent = ok ? 'Online' : 'Offline';
}

// ── preset labels ─────────────────────────────────────────────────────
function updatePresetLabels() {
  document.getElementById('pc1val').textContent = fmtSec(t1);
  document.getElementById('pc2val').textContent = fmtSec(t2);
}
function fmtSec(s) {
  var m = Math.floor(s/60), r = s%60;
  return String(m).padStart(2,'0') + ':' + String(r).padStart(2,'0');
}

// ── preset start ─────────────────────────────────────────────────────
function presetStart(n) {
  if (motorOn) { cmd('/api/off'); return; }
  cmd('/api/on?dur=' + (n===1 ? t1 : t2));
}

// ── main toggle ──────────────────────────────────────────────────────
function toggleMotor() {
  if (motorOn) { cmd('/api/off'); }
  else         { cmd('/api/on?dur=0'); }
}

// ── custom start ─────────────────────────────────────────────────────
function startCustom() {
  var v = parseInt(document.getElementById('durSlider').value);
  cmd('/api/on?dur=' + (v * 60));
}

// ── send command ─────────────────────────────────────────────────────
function cmd(url) {
  fetch(url).then(function(){setTimeout(poll,300);}).catch(function(){});
}

// ── slider ───────────────────────────────────────────────────────────
function initSlider() {
  var s = document.getElementById('durSlider');
  sliderMove(s);
}
function sliderMove(s) {
  var v = parseInt(s.value);
  var pct = ((v-1)/(120-1)*100).toFixed(1) + '%';
  s.style.setProperty('--pct', pct);
  document.getElementById('sliderVal').textContent = fmtSec(v * 60);
}

// ══════════════════════════════════════════════════════════════════════
//  SCROLL DRUM PICKER
// ══════════════════════════════════════════════════════════════════════
var activePicker = 1; // 1 or 2
var pickMin = {1:2, 2:5}, pickSec = {1:0, 2:0};

function buildDrums() {
  var dm = document.getElementById('drumMin');
  var ds = document.getElementById('drumSec');
  dm.innerHTML = '<div class="drum-pad"></div>';
  ds.innerHTML = '<div class="drum-pad"></div>';
  for (var i=0; i<=59; i++) {
    var mi=document.createElement('div'); mi.className='drum-item'; mi.textContent=String(i).padStart(2,'0'); mi.dataset.v=i; dm.appendChild(mi);
    var si=document.createElement('div'); si.className='drum-item'; si.textContent=String(i).padStart(2,'0'); si.dataset.v=i; ds.appendChild(si);
  }
  dm.innerHTML += '<div class="drum-pad"></div>';
  ds.innerHTML += '<div class="drum-pad"></div>';
}

function openPicker(n) {
  activePicker = n;
  document.getElementById('tab1').className = 'pick-tab' + (n===1?' active':'');
  document.getElementById('tab2').className = 'pick-tab' + (n===2?' active':'');
  document.getElementById('pickerTitle').textContent = n===1 ? 'Set 1× Timer' : 'Set 2× Timer';
  var m = pickMin[n], s = pickSec[n];
  scrollDrumTo('drumMin', m);
  scrollDrumTo('drumSec', s);
  highlightDrum('drumMin', m);
  highlightDrum('drumSec', s);
  document.getElementById('pickerOverlay').classList.add('show');
}

function switchTab(n) {
  // save current
  pickMin[activePicker] = getScrollVal('drumMin');
  pickSec[activePicker] = getScrollVal('drumSec');
  activePicker = n;
  document.getElementById('tab1').className = 'pick-tab' + (n===1?' active':'');
  document.getElementById('tab2').className = 'pick-tab' + (n===2?' active':'');
  document.getElementById('pickerTitle').textContent = n===1 ? 'Set 1× Timer' : 'Set 2× Timer';
  scrollDrumTo('drumMin', pickMin[n]);
  scrollDrumTo('drumSec', pickSec[n]);
  highlightDrum('drumMin', pickMin[n]);
  highlightDrum('drumSec', pickSec[n]);
}

function scrollDrumTo(id, v) {
  var d = document.getElementById(id);
  d.scrollTop = v * 56;
}

function getScrollVal(id) {
  var d = document.getElementById(id);
  return Math.round(d.scrollTop / 56);
}

var drumScrollTimers = {};
function drumScroll(id) {
  clearTimeout(drumScrollTimers[id]);
  drumScrollTimers[id] = setTimeout(function() {
    var v = getScrollVal(id);
    var d = document.getElementById(id);
    d.scrollTop = v * 56; // snap
    highlightDrum(id, v);
  }, 80);
}

function highlightDrum(id, v) {
  var items = document.getElementById(id).querySelectorAll('.drum-item');
  items.forEach(function(it) {
    it.className = 'drum-item' + (parseInt(it.dataset.v)===v?' sel':'');
  });
}

function pickerDone() {
  var m = getScrollVal('drumMin');
  var s = getScrollVal('drumSec');
  pickMin[activePicker] = m;
  pickSec[activePicker] = s;
  var total = m*60 + s;
  if (total < 10) { alert('Minimum 10 seconds'); return; }
  if (activePicker===1) t1 = total;
  else                  t2 = total;
  updatePresetLabels();
  // save to device
  fetch('/api/setTimers?t1='+t1+'&t2='+t2).catch(function(){});
  closePicker();
}

function overlayClick(e) {
  if (e.target === document.getElementById('pickerOverlay')) closePicker();
}
function closePicker() {
  document.getElementById('pickerOverlay').classList.remove('show');
}

// ── offline overlay ──────────────────────────────────────────────────
var offlineActive = false;
function showOffline() {
  if (offlineActive) return;
  offlineActive = true;
  document.getElementById('offlineOverlay').classList.add('show');
  startRetryCountdown(10);
}
function hideOffline() {
  if (!offlineActive) return;
  offlineActive = false;
  clearRetryCountdown();
  document.getElementById('offlineOverlay').classList.remove('show');
}
function startRetryCountdown(n) {
  clearRetryCountdown();
  retryCount = n;
  updateCountLabel();
  retryTimer = setInterval(function() {
    retryCount--;
    if (retryCount <= 0) { clearRetryCountdown(); poll(); startRetryCountdown(10); }
    else { updateCountLabel(); }
  }, 1000);
}
function clearRetryCountdown() {
  if (retryTimer) { clearInterval(retryTimer); retryTimer = null; }
}
function updateCountLabel() {
  document.getElementById('offlineCount').textContent = 'Retrying in ' + retryCount + 's…';
}
function retryNow() {
  clearRetryCountdown();
  document.getElementById('offlineCount').textContent = 'Connecting…';
  setTimeout(function() { poll(); if (offlineActive) startRetryCountdown(10); }, 400);
}
</script>
</body>
</html>
)HTMLEOF";

// ── API route helpers ──────────────────────────────────────────────────
void sendJSON(const String& body) {
  g_server.sendHeader("Access-Control-Allow-Origin", "*");
  g_server.send(200, "application/json", body);
}

String statusJSON() {
  bool wok = (WiFi.status() == WL_CONNECTED);
  uint32_t rem = motorRemaining();
  String j = "{";
  j += "\"motor\":"  + String(g_motorOn   ? "true" : "false") + ",";
  j += "\"dur\":"    + String(g_motorDurSec) + ",";
  j += "\"rem\":"    + String(rem)          + ",";
  j += "\"wifi\":"   + String(wok ? "true" : "false") + ",";
  j += "\"t1\":"     + String(g_timer1)    + ",";
  j += "\"t2\":"     + String(g_timer2)    + "}";
  return j;
}

// ── HTTP server routes ─────────────────────────────────────────────────
void setupServer() {
  // Serve main page
  g_server.on("/", []() {
    g_server.send_P(200, "text/html", HTML_PAGE);
  });

  // Status JSON
  g_server.on("/api/status", []() {
    sendJSON(statusJSON());
  });

  // Turn ON with optional duration
  g_server.on("/api/on", []() {
    uint32_t dur = 0;
    if (g_server.hasArg("dur")) dur = g_server.arg("dur").toInt();
    motorStart(dur);
    g_forceOff = false;
    sendJSON("{\"ok\":1}");
  });

  // Turn OFF
  g_server.on("/api/off", []() {
    motorsSet(false);
    sendJSON("{\"ok\":1}");
  });

  // Update timer defaults
  g_server.on("/api/setTimers", []() {
    if (g_server.hasArg("t1")) g_timer1 = constrain((uint32_t)g_server.arg("t1").toInt(), 10, 3600);
    if (g_server.hasArg("t2")) g_timer2 = constrain((uint32_t)g_server.arg("t2").toInt(), 10, 7200);
    sendJSON("{\"ok\":1}");
  });

  // Static offline page (for bookmarking / direct access when device is unreachable)
  g_server.on("/offline", []() {
    g_server.send_P(200, "text/html", R"EOF(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Motor Controller – Offline</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#f2f0eb;font-family:'Helvetica Neue',sans-serif;display:flex;align-items:center;
  justify-content:center;min-height:100vh;flex-direction:column;padding:32px;text-align:center}
svg{width:160px;height:140px;margin-bottom:28px}
.pump-body{animation:float 3s ease-in-out infinite}
@keyframes float{0%,100%{transform:translateY(0)}50%{transform:translateY(-7px)}}
.arc{stroke-dasharray:80;animation:fade 2s ease-in-out infinite}
.a1{animation-delay:.6s}.a2{animation-delay:.3s}.a3{animation-delay:0s}
@keyframes fade{0%,100%{opacity:.1}50%{opacity:1}}
.pr{animation:pr 2s ease-out infinite}
@keyframes pr{0%{r:16;opacity:.7}100%{r:34;opacity:0}}
h1{font-size:22px;font-weight:500;margin-bottom:8px;color:#1a1a1a}
p{font-size:14px;color:#999;line-height:1.7;margin-bottom:28px}
a{display:inline-block;padding:14px 36px;background:#1a1a1a;color:#f2f0eb;border-radius:14px;
  font-size:13px;font-weight:500;letter-spacing:.06em;text-decoration:none;text-transform:uppercase}
.logo{margin-top:40px;opacity:.6}
.logo img{height:28px;width:auto}
</style></head><body>
<svg viewBox="0 0 160 140">
  <circle cx="80" cy="95" r="16" fill="none" stroke="#e74c3c" stroke-width="2" class="pr"/>
  <g class="pump-body">
    <rect x="54" y="72" width="52" height="48" rx="10" fill="#1a1a1a"/>
    <rect x="66" y="28" width="28" height="50" rx="6" fill="#1a1a1a"/>
    <rect x="62" y="72" width="36" height="8" rx="4" fill="#333"/>
    <circle cx="80" cy="98" r="14" fill="#f2f0eb"/>
    <line x1="73" y1="91" x2="87" y2="105" stroke="#e74c3c" stroke-width="3" stroke-linecap="round"/>
    <line x1="87" y1="91" x2="73" y2="105" stroke="#e74c3c" stroke-width="3" stroke-linecap="round"/>
    <rect x="48" y="91" width="7" height="3" rx="1.5" fill="#333"/>
    <rect x="48" y="98" width="7" height="3" rx="1.5" fill="#333"/>
    <rect x="105" y="91" width="7" height="3" rx="1.5" fill="#333"/>
    <rect x="105" y="98" width="7" height="3" rx="1.5" fill="#333"/>
  </g>
  <path class="arc a3" d="M38 44 Q80 8 122 44" stroke="#e74c3c" stroke-width="3" fill="none" stroke-linecap="round"/>
  <path class="arc a2" d="M50 54 Q80 26 110 54" stroke="#e74c3c" stroke-width="3" fill="none" stroke-linecap="round"/>
  <path class="arc a1" d="M63 64 Q80 48 97 64" stroke="#e74c3c" stroke-width="3" fill="none" stroke-linecap="round"/>
  <path d="M16 132 Q40 126 64 132 Q88 138 112 132 Q136 126 150 132" stroke="#ccc" stroke-width="2" fill="none" stroke-linecap="round"/>
</svg>
<h1>Device Unreachable</h1>
<p>Motor Controller is not<br>connected to WiFi network.<br>Check that the device is powered on.</p>
<a href="/">Try Again</a>
<div class="logo">
  <p style="font-size:10px;color:#ccc;letter-spacing:.1em;text-transform:uppercase;margin:0 0 6px">Powered by</p>
  <a href="https://elevatebharat.com" target="_blank" rel="noopener" style="background:none;padding:0;display:inline">
    <img src="https://elevatebharat.com/files/files/static/img/landing/intro/ElevateBharat_Logo.png" alt="ElevateBharat">
  </a>
</div>
</body></html>)EOF");
  });

  g_server.begin();
  Serial.println("[HTTP] Server started");
}

// ══════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== Motor Controller Boot ===");

  // Set relay pins — active LOW, start OFF
  uint8_t relays[] = { PIN_RELAY_WIFI, PIN_RELAY_M1, PIN_RELAY_M2, PIN_RELAY_M3 };
  for (uint8_t i = 0; i < 4; i++) { pinMode(relays[i], OUTPUT); digitalWrite(relays[i], RELAY_OFF); }

  // Button — internal pull-up
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.config(staticIP, gateway, subnet, dns1);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  setWifiState(WF_CONNECTING);
  Serial.print("[WIFI] Connecting");

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    yield(); delay(200); Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    setWifiState(WF_CONNECTED);
  } else {
    Serial.println("\n[WIFI] Not reachable — offline mode");
    setWifiState(WF_DROPPED);
  }

  setupServer();
}

void loop() {
  // ── WiFi watchdog ───────────────────────────────────────────────────
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && g_wifiState != WF_CONNECTED) {
    setWifiState(WF_CONNECTED);
  } else if (!connected && g_wifiState == WF_CONNECTED) {
    setWifiState(WF_DROPPED);
  }

  if (connected) {
    g_server.handleClient();
  }

  // ── Auto-off timer ──────────────────────────────────────────────────
  if (g_motorOn && g_motorDurSec > 0) {
    if (motorRemaining() == 0) {
      Serial.println("[MOTOR] Timer expired → OFF");
      motorsSet(false);
    }
  }

  // ── Button ──────────────────────────────────────────────────────────
  buttonTick();

  yield();
}
