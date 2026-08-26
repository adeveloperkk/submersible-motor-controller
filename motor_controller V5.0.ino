/*
=====================================================================================
  SUBMERSIBLE MOTOR CONTROLLER — ESP8266 NodeMCU by Developerkk (www.developerkk.in)
=====================================================================================

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
  1 press  → motor ON for DEFAULT_TIME_1  (default 5 min)
  2 presses within 600 ms → motor ON for DEFAULT_TIME_2  (default 10 min)
  Hold 3 s → FORCE OFF  (overrides WiFi command, clears timer)

  While motor is ON a button press first checks for hold:
    • Quick press  → turns motor OFF (toggle)
    • Hold 3 s    → FORCE OFF (same as above)

  WEB UI  http://192.168.1.51/
  ─────────────────────────────────────────────────────────────
  • Countdown ring with MM:SS display
  • Two preset buttons (5-min / 10-min) matching physical button
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
const char* WIFI_SSID     = "Wifi SSID";
const char* WIFI_PASSWORD = "WIFI Password";

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
uint32_t g_timer1 = 300;   // single press  = 5 min
uint32_t g_timer2 = 600;   // double press  = 10 min

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
.footer-ip{font-size:11px;color:#bbb;letter-spacing:.08em}

/* ── unlimited-run button (bottom-left, password protected) ── */
.unlimited-btn{position:fixed;left:20px;bottom:20px;width:46px;height:46px;border-radius:50%;
  background:#1a1a1a;color:#f2f0eb;border:none;display:flex;align-items:center;justify-content:center;
  font-size:18px;font-weight:300;cursor:pointer;box-shadow:0 4px 14px rgba(0,0,0,.25);z-index:50;
  transition:transform .12s}
.unlimited-btn:active{transform:scale(.92)}

/* ── in-app modal popups (confirm / password) ── */
.modal-overlay{position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:200;
  display:flex;align-items:center;justify-content:center;opacity:0;pointer-events:none;transition:opacity .2s}
.modal-overlay.show{opacity:1;pointer-events:all}
.modal-card{width:86%;max-width:320px;background:#fff;border-radius:20px;padding:24px 22px 20px;
  box-shadow:0 20px 60px rgba(0,0,0,.3);transform:scale(.92);transition:transform .2s}
.modal-overlay.show .modal-card{transform:scale(1)}
.modal-title{font-size:16px;font-weight:600;color:#1a1a1a;margin-bottom:8px}
.modal-msg{font-size:13px;color:#777;line-height:1.5;margin-bottom:18px}
.modal-input{width:100%;padding:12px 14px;border-radius:12px;border:1.5px solid #e0ddd6;
  font-size:18px;letter-spacing:.3em;text-align:center;margin-bottom:8px;outline:none;
  font-variant-numeric:tabular-nums}
.modal-input:focus{border-color:#1a1a1a}
.modal-error{font-size:12px;color:#e74c3c;margin-bottom:4px;min-height:14px}
.modal-actions{display:flex;gap:10px;margin-top:6px}
.modal-btn{flex:1;padding:12px;border-radius:12px;font-size:13px;font-weight:500;
  letter-spacing:.05em;text-transform:uppercase;cursor:pointer;border:none}
.modal-btn-cancel{background:#f2f0eb;color:#888}
.modal-btn-confirm{background:#1a1a1a;color:#f2f0eb}
.modal-btn-confirm.danger{background:#e74c3c}
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
      <div class="pcard-val" id="pc1val">5:00</div>
      <div class="pcard-label">Default · 1×</div>
      <div class="pcard-hint">Single press</div>
    </div>
    <div class="pcard" id="pc2" onclick="presetStart(2)">
      <button class="edit-btn" onclick="event.stopPropagation();openPicker(2)" title="Edit">✎</button>
      <div class="pcard-icon">◎</div>
      <div class="pcard-val" id="pc2val">10:00</div>
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
    <input type="range" id="durSlider" min="1" max="20" step="1" value="3"
      oninput="sliderMove(this)">
    <button class="start-custom" onclick="startCustom()">▶ Start with this timer</button>
  </div>

  <!-- footer -->
  <div class="footer">
    <div class="footer-ip">192.168.1.51</div>
    <div class="footer-powered">
      <span>Powered by</span>
      Elevate Bharat
    </div>
  </div>


  <!-- unlimited-run button (password protected) -->
  <button class="unlimited-btn" onclick="startUnlimited()" title="Unlimited run (password required)">∞</button>
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

<!-- ── confirm modal (used for all "start" actions) ────────────────────── -->
<div class="modal-overlay" id="confirmOverlay" onclick="modalOverlayClick(event,'confirmOverlay',closeConfirmModal)">
  <div class="modal-card">
    <div class="modal-title" id="confirmTitle">Start Motor</div>
    <div class="modal-msg" id="confirmMsg">Start motor now?</div>
    <div class="modal-actions">
      <button class="modal-btn modal-btn-cancel" onclick="closeConfirmModal()">Cancel</button>
      <button class="modal-btn modal-btn-confirm" id="confirmOkBtn" onclick="confirmModalOk()">Start</button>
    </div>
  </div>
</div>

<!-- ── password modal (unlimited-run gate) ─────────────────────────────── -->
<div class="modal-overlay" id="passwordOverlay" onclick="modalOverlayClick(event,'passwordOverlay',closePasswordModal)">
  <div class="modal-card">
    <div class="modal-title">Unlimited Run</div>
    <div class="modal-msg">Enter password to continue</div>
    <input type="password" class="modal-input" id="passwordInput" inputmode="numeric"
      maxlength="8" placeholder="••••" onkeydown="if(event.key==='Enter')passwordModalOk()">
    <div class="modal-error" id="passwordError"></div>
    <div class="modal-actions">
      <button class="modal-btn modal-btn-cancel" onclick="closePasswordModal()">Cancel</button>
      <button class="modal-btn modal-btn-confirm" onclick="passwordModalOk()">Continue</button>
    </div>
  </div>
</div>

<script>
// ── state ───────────────────────────────────────────────────────────
var t1 = 300, t2 = 600;          // timer defaults in seconds (5 min / 10 min)
var motorOn = false, motorDur = 0, remaining = 0;
var wifiOk = false;
var pollTimer = null, countTimer = null;
var localRemaining = 0, localDur = 0;
const C = 2 * Math.PI * 85;      // 534.07  (circumference)

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
    motorOn   = d.motor;
    motorDur  = d.dur;
    remaining = d.rem;
    wifiOk    = d.wifi;
    t1 = d.t1; t2 = d.t2;
    updatePresetLabels();
    applyStatus();
    // restart local countdown from server truth
    if (motorOn && motorDur > 0) { startLocalCount(remaining, motorDur); }
    else if (!motorOn)           { clearLocalCount(); }
  }).catch(function() {
    // Offline — keep ticking from local state
    setWifiDot(false);
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
  var dur = (n===1 ? t1 : t2);
  showConfirmModal('Start Motor', 'Start motor for ' + fmtSec(dur) + '?', function() {
    cmd('/api/on?dur=' + dur);
  });
}

// ── main toggle ──────────────────────────────────────────────────────
// "Start Motor" always runs a fixed 15-minute timer. Stopping never asks.
var MAIN_BTN_DUR = 900; // 15 min
function toggleMotor() {
  if (motorOn) { cmd('/api/off'); return; }
  showConfirmModal('Start Motor', 'Start motor for 15 minutes?', function() {
    cmd('/api/on?dur=' + MAIN_BTN_DUR);
  });
}

// ── custom start ─────────────────────────────────────────────────────
function startCustom() {
  var v = parseInt(document.getElementById('durSlider').value);
  showConfirmModal('Start Motor', 'Start motor for ' + fmtSec(v * 60) + '?', function() {
    cmd('/api/on?dur=' + (v * 60));
  });
}

// ── unlimited run (password protected) ─────────────────────────────────
var UNLIMITED_PASSWORD = '2560';
function startUnlimited() {
  if (motorOn) { cmd('/api/off'); return; }
  showPasswordModal(function() {
    showConfirmModal('Unlimited Run', 'Start motor for UNLIMITED time?', function() {
      cmd('/api/on?dur=0');
    }, true);
  });
}

// ══════════════════════════════════════════════════════════════════════
//  IN-APP MODAL POPUPS  (replace browser confirm()/prompt())
// ══════════════════════════════════════════════════════════════════════

// generic click-outside-to-close for any modal overlay
function modalOverlayClick(e, id, closeFn) {
  if (e.target === document.getElementById(id)) closeFn();
}

// ── confirm modal ──────────────────────────────────────────────────────
var pendingConfirmCb = null;
function showConfirmModal(title, msg, onConfirm, danger) {
  document.getElementById('confirmTitle').textContent = title;
  document.getElementById('confirmMsg').textContent = msg;
  var btn = document.getElementById('confirmOkBtn');
  btn.className = 'modal-btn modal-btn-confirm' + (danger ? ' danger' : '');
  pendingConfirmCb = onConfirm;
  document.getElementById('confirmOverlay').classList.add('show');
}
function closeConfirmModal() {
  document.getElementById('confirmOverlay').classList.remove('show');
  pendingConfirmCb = null;
}
function confirmModalOk() {
  var cb = pendingConfirmCb;
  closeConfirmModal();
  if (cb) cb();
}

// ── password modal ──────────────────────────────────────────────────────
var pendingPasswordCb = null;
function showPasswordModal(onSuccess) {
  document.getElementById('passwordInput').value = '';
  document.getElementById('passwordError').textContent = '';
  pendingPasswordCb = onSuccess;
  document.getElementById('passwordOverlay').classList.add('show');
  setTimeout(function() { document.getElementById('passwordInput').focus(); }, 150);
}
function closePasswordModal() {
  document.getElementById('passwordOverlay').classList.remove('show');
  pendingPasswordCb = null;
}
function passwordModalOk() {
  var val = document.getElementById('passwordInput').value;
  if (val !== UNLIMITED_PASSWORD) {
    document.getElementById('passwordError').textContent = 'Incorrect password';
    return;
  }
  var cb = pendingPasswordCb;
  closePasswordModal();
  if (cb) cb();
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
  var pct = ((v-1)/(20-1)*100).toFixed(1) + '%';
  s.style.setProperty('--pct', pct);
  document.getElementById('sliderVal').textContent = fmtSec(v * 60);
}

// ══════════════════════════════════════════════════════════════════════
//  SCROLL DRUM PICKER
// ══════════════════════════════════════════════════════════════════════
var activePicker = 1; // 1 or 2
var pickMin = {1:5, 2:10}, pickSec = {1:0, 2:0};

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
