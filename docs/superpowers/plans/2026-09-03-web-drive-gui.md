# Web-Based Drive & Telemetry GUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A landscape browser page, served directly from the ESP32 over its own Wi-Fi AP, that drives the robot (joystick + speed cap) and shows live motor-current/bus-voltage telemetry, styled as a two-panel sci-fi HUD.

**Architecture:** A new Arduino sketch (`embedded/web_drive/`) reuses the already-bench-verified motor/current-sensing logic from `drive_test.ino`, adds Wi-Fi AP + `esp_http_server` (static file serving from LittleFS, plus a WebSocket endpoint for drive commands in and telemetry out), a voltage-based duty clamp (from `drive_calib.ino`) and a 400ms command watchdog. The frontend is vanilla HTML/CSS/JS, no build step, with one pure function (joystick → duty mixing) factored out and unit-tested with Node.

**Tech Stack:** Arduino-ESP32 core 3.3.11 (`esp_http_server`, `LittleFS`, `WiFi`, `ESPmDNS`), `Adafruit_INA219` (already a dependency of `drive_test.ino`). Frontend: vanilla HTML/CSS/JS, SVG for gauges, Node (`assert`, built-in) for the one unit test.

**Spec:** `docs/superpowers/specs/2026-09-03-web-drive-gui-design.md`

## Global Constraints

- Pin map (unchanged from `drive_test.ino`/`drive_calib.ino`): `ENA=25, IN1=26, IN2=27, ENB=32, IN3=33, IN4=14`; I2C `SDA=21, SCL=22`; INA219 addresses `LEFT=0x40, RIGHT=0x41`.
- Duty clamp: `V_MOTOR_MAX=5.5V`; default constants `v0Drop=1.9V, rTot=2.5Ω, vBusManual=12.6V` (from `drive_calib.ino`, not yet a real calibration — see design doc §9).
- Command watchdog: `400ms` — no valid `drive` message in that window forces `drive(0,0)`.
- Telemetry push rate: `~5Hz` (`200ms`).
- Wire protocol — inbound: `{"type":"drive","left":N,"right":N}`, `N` ∈ `[-255,255]`. Outbound: `{"type":"telemetry","left_a":F,"right_a":F,"left_duty":N,"right_duty":N,"vbus":F}`.
- No JS framework, no build step (design D6) — plain `<script>` tags, no bundler.
- `drive_test.ino` and `drive_calib.ino` are not modified by this plan (design D1/D4).
- Wheels off the ground for all bring-up testing until direction/watchdog are confirmed (same rule `drive_test.ino` already states).

---

## File Structure

```
embedded/web_drive/
  web_drive.ino      setup()/loop(): Wi-Fi AP, mDNS, wires drive_core.h + ws_handler.h together
  drive_core.h        pin map, drive/current-sensing logic, duty clamp (adapted from drive_test.ino + drive_calib.ino)
  ws_handler.h        esp_http_server: static file serving (LittleFS), /ws WebSocket handler, JSON parse/build, watchdog
  data/
    index.html         two-panel landscape layout (control left, telemetry right)
    style.css          HUD styling (dark ground, teal/orange glow, status ring, gauges)
    mix.js              pure joystick->duty mixing function (dual-use: browser global + Node module)
    mix.test.js          Node unit test for mix.js
    app.js               WebSocket client, joystick pointer handling, telemetry rendering
```

`drive_core.h`/`ws_handler.h` are header-only (`static` functions), included exactly once from `web_drive.ino` — standard Arduino multi-file-sketch pattern, no `.cpp`/linkage complications.

---

## Task 1: `drive_core.h` — motor drive + current sensing + duty clamp

**Files:**
- Create: `embedded/web_drive/drive_core.h`

**Interfaces:**
- Consumes: nothing (leaf module)
- Produces: `driveSetup()`, `drive(int left, int right)`, `stopAll()`, `readCurrents()`, `dutyMaxCounts()`, `readBusVoltage()`, arrays `isense[2]` (`CurrentSensor`, fields `.name`, `.amps`, `.present`), `lastDuty[2]` (`int`), constants `LEFT=0`, `RIGHT=1`

- [ ] **Step 1: Write `drive_core.h`**

```cpp
#pragma once

// drive_core.h - motor drive + current sensing + duty clamp.
// Pin map and drive logic copied from embedded/drive_test/drive_test.ino
// (bench-verified, untouched by this plan). Duty clamp copied from
// embedded/drive_calib/drive_calib.ino - drive_test.ino has no such clamp,
// which docs/superpowers/specs/2026-09-03-incline-compensation-design.md
// §3.1 flags as a live over-voltage risk (motors rated 3.0-6.0V on a 12V
// bus). This sketch does not reintroduce that gap.

#include <Wire.h>
#include <Adafruit_INA219.h>

// ---------------------------------------------------------------- pin map
#define ENA 25
#define IN1 26
#define IN2 27
#define ENB 32
#define IN3 33
#define IN4 14

#define I2C_SDA 21
#define I2C_SCL 22
#define VBUS_ADC_PIN 34

static const int PWM_FREQ_HZ = 1000;
static const int PWM_BITS    = 8;

// ------------------------------------------------------- voltage/duty clamp
// Motors are rated 3.0-6.0V; the L298N is fed 12V. V_MOTOR_MAX=5.5V leaves
// margin below the 6.0V datasheet max for error in v0Drop/rTot. These
// v0Drop/rTot values are drive_calib.ino's DEFAULTS, not a measured
// calibration (none exists in this repo yet) - see design doc §9.
static const float V_MOTOR_MAX = 5.5f;
static float vBusManual = 12.6f;
static float vbusScale  = 5.545f;   // (100k + 22k) / 22k
static bool  useDivider = false;    // no GPIO34 divider fitted yet
static float v0Drop = 1.9f;
static float rTot   = 2.5f;

static float readBusVoltage() {
  if (!useDivider) return vBusManual;
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(VBUS_ADC_PIN);
  return (acc / 16.0f / 1000.0f) * vbusScale;
}

static int dutyMaxCounts() {
  float head = readBusVoltage() - v0Drop;
  if (head <= 0.1f) return 0;
  float d = V_MOTOR_MAX / head;
  if (d > 1.0f) d = 1.0f;
  if (d < 0.0f) d = 0.0f;
  return (int)(d * 255.0f);
}

// ---------------------------------------------------------------- drive
struct Side {
  const char *name;
  int  en, inFwd, inRev;
};

static Side sides[2] = {
  { "LEFT",  ENB, IN4, IN3 },   // OUT4 = +, OUT3 = -
  { "RIGHT", ENA, IN2, IN1 },   // OUT2 = +, OUT1 = -
};
static const int LEFT = 0, RIGHT = 1;

static int lastDuty[2] = { 0, 0 };

static void pwmSetup(int pin, int ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcAttach(pin, PWM_FREQ_HZ, PWM_BITS);
#else
  ledcSetup(ch, PWM_FREQ_HZ, PWM_BITS);
  ledcAttachPin(pin, ch);
#endif
}

static void pwmWrite(int pin, int ch, int duty) {
  int cap = dutyMaxCounts();
  if (duty > cap) duty = cap;
  if (duty < 0)   duty = 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(ch, duty);
#endif
}

static void setSide(int s, int duty) {
  Side &sd = sides[s];
  bool fwd = duty >= 0;
  int  mag = abs(duty);
  if (mag > 255) mag = 255;
  if (mag == 0) {
    digitalWrite(sd.inFwd, LOW);
    digitalWrite(sd.inRev, LOW);
  } else {
    digitalWrite(sd.inFwd, fwd ? HIGH : LOW);
    digitalWrite(sd.inRev, fwd ? LOW  : HIGH);
  }
  pwmWrite(sd.en, s, mag);
  lastDuty[s] = mag;
}

static void drive(int left, int right) {
  setSide(LEFT,  left);
  setSide(RIGHT, right);
}

static void stopAll() { drive(0, 0); }

// ------------------------------------------------------- current sensing
static Adafruit_INA219 ina_left (0x40);
static Adafruit_INA219 ina_right(0x41);

struct CurrentSensor {
  const char      *name;
  Adafruit_INA219 *dev;
  uint8_t          addr;
  bool             present;
  bool             invert;
  float            amps;
};

static CurrentSensor isense[2] = {
  { "LEFT",  &ina_left,  0x40, true, false, 0.0f },
  { "RIGHT", &ina_right, 0x41, true, false, 0.0f },
};

static void sensorsBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.dev->begin()) {
      cs.present = false;
      Serial.printf("INA219 %-5s : NOT FOUND at 0x%02X\n", cs.name, cs.addr);
      continue;
    }
    cs.dev->setCalibration_32V_2A();
    Serial.printf("INA219 %-5s : ok at 0x%02X\n", cs.name, cs.addr);
  }
}

static void readCurrents() {
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.present) { cs.amps = NAN; continue; }
    float a = cs.dev->getCurrent_mA() / 1000.0f;
    if (cs.invert) a = -a;
    cs.amps = a;
  }
}

// ---------------------------------------------------------------- lifecycle
static void driveSetup() {
  // Kill both H-bridges FIRST, before anything else can run.
  int enPins[2] = { ENA, ENB };
  for (int i = 0; i < 2; i++) {
    pinMode(enPins[i], OUTPUT);
    digitalWrite(enPins[i], LOW);
  }
  for (int s = 0; s < 2; s++) {
    pinMode(sides[s].inFwd, OUTPUT);
    pinMode(sides[s].inRev, OUTPUT);
    digitalWrite(sides[s].inFwd, LOW);
    digitalWrite(sides[s].inRev, LOW);
    pwmSetup(sides[s].en, s);
  }
  stopAll();
  sensorsBegin();
}
```

- [ ] **Step 2: Manual verification (no local ESP32 toolchain in this environment)**

Open the file in the Arduino IDE as part of the `web_drive` sketch folder (it won't compile standalone yet — Task 3 adds the `.ino` that includes it). Check by inspection against `embedded/drive_test/drive_test.ino`: pin numbers identical, `setSide()`/`drive()`/`readCurrents()` logic identical modulo the added `dutyMaxCounts()` clamp inside `pwmWrite()`. Full compile happens in Task 3's verification step.

- [ ] **Step 3: Commit**

```bash
git add embedded/web_drive/drive_core.h
git commit -m "Add web_drive drive_core.h (motor drive + duty clamp)"
```

---

## Task 2: `ws_handler.h` — HTTP/WebSocket server, JSON, watchdog

**Files:**
- Create: `embedded/web_drive/ws_handler.h`

**Interfaces:**
- Consumes (from Task 1's `drive_core.h`): `drive(int,int)`, `stopAll()`, `readCurrents()`, `readBusVoltage()`, `isense[2].amps`, `lastDuty[2]`, `LEFT`, `RIGHT`
- Produces: `startWebServer()` (returns `bool`), `wsWatchdogCheck()`, `wsSendTelemetry()`

- [ ] **Step 1: Write `ws_handler.h`**

```cpp
#pragma once

// ws_handler.h - esp_http_server: static file serving from LittleFS, the
// /ws WebSocket endpoint (drive commands in, telemetry out), and the
// 400ms command watchdog. Depends on drive_core.h being included first.

#include <esp_http_server.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>

static httpd_handle_t server = NULL;
static int wsClientFd = -1;
static uint32_t lastDriveMsgMs = 0;
static const uint32_t WATCHDOG_MS = 400;

// ---- inbound: {"type":"drive","left":N,"right":N} ----
// Hand-rolled parsing, not a JSON library: the message shape is fixed and
// produced only by our own frontend (data/app.js), so this is sufficient
// and avoids a new library dependency (design D6's minimal-dependency
// spirit extended to the wire format). A malformed or out-of-range message
// returns false and is dropped silently - the watchdog is NOT reset, so a
// garbled command degrades to "eventually stops," never "drives on bad
// data."
static bool parseDriveMessage(const char *json, int *left, int *right) {
  const char *lp = strstr(json, "\"left\"");
  const char *rp = strstr(json, "\"right\"");
  if (!lp || !rp) return false;

  lp = strchr(lp, ':');
  rp = strchr(rp, ':');
  if (!lp || !rp) return false;

  long l = strtol(lp + 1, nullptr, 10);
  long r = strtol(rp + 1, nullptr, 10);
  if (l < -255 || l > 255 || r < -255 || r > 255) return false;

  *left  = (int)l;
  *right = (int)r;
  return true;
}

static esp_err_t wsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    // Handshake call - no frame to read yet.
    wsClientFd = httpd_req_to_sockfd(req);
    Serial.println("WS client connected");
    return ESP_OK;
  }

  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type = HTTPD_WS_TYPE_TEXT;

  // First call with a NULL/zero-length buffer just reports pkt.len.
  esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
  if (ret != ESP_OK) return ret;
  if (pkt.len == 0) return ESP_OK;

  static uint8_t buf[257];
  size_t recvLen = pkt.len;
  if (recvLen >= sizeof(buf)) recvLen = sizeof(buf) - 1;  // truncate defensively

  pkt.payload = buf;
  ret = httpd_ws_recv_frame(req, &pkt, recvLen);
  if (ret != ESP_OK) return ret;

  size_t n = pkt.len;
  if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
  buf[n] = '\0';

  if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
    Serial.println("WS client disconnected");
    wsClientFd = -1;
    stopAll();
    return ESP_OK;
  }

  if (pkt.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

  int left, right;
  if (parseDriveMessage((const char *)buf, &left, &right)) {
    drive(left, right);
    lastDriveMsgMs = millis();
  }
  return ESP_OK;
}

// Command watchdog: call every loop() iteration. No valid drive message
// within WATCHDOG_MS -> force stop. Covers both a clean disconnect (handled
// above too, for a faster stop) and a silent connection loss (stalled Wi-Fi,
// crashed browser tab) where no CLOSE frame ever arrives.
static void wsWatchdogCheck() {
  if (millis() - lastDriveMsgMs > WATCHDOG_MS) {
    stopAll();
  }
}

// Push telemetry to the connected client, if any. Uses the async send path
// (needs only the socket fd, not a live httpd_req_t) since this is called
// from loop(), not from inside a request handler.
static void wsSendTelemetry() {
  if (wsClientFd < 0) return;

  readCurrents();

  char json[192];
  int n = snprintf(json, sizeof(json),
    "{\"type\":\"telemetry\",\"left_a\":%.3f,\"right_a\":%.3f,"
    "\"left_duty\":%d,\"right_duty\":%d,\"vbus\":%.2f}",
    isense[LEFT].amps, isense[RIGHT].amps,
    lastDuty[LEFT], lastDuty[RIGHT], readBusVoltage());

  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type    = HTTPD_WS_TYPE_TEXT;
  pkt.payload = (uint8_t *)json;
  pkt.len     = n;

  esp_err_t ret = httpd_ws_send_frame_async(server, wsClientFd, &pkt);
  if (ret != ESP_OK) {
    wsClientFd = -1;  // client is gone - stop trying; watchdog stops the motors
  }
}

// ---- static file serving (LittleFS) ----
static esp_err_t serveFile(httpd_req_t *req, const char *path, const char *contentType) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, contentType);
  uint8_t buf[512];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    httpd_resp_send_chunk(req, (const char *)buf, n);
  }
  f.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t indexHandler(httpd_req_t *req) { return serveFile(req, "/index.html", "text/html"); }
static esp_err_t cssHandler(httpd_req_t *req)   { return serveFile(req, "/style.css", "text/css"); }
static esp_err_t mixJsHandler(httpd_req_t *req) { return serveFile(req, "/mix.js", "application/javascript"); }
static esp_err_t appJsHandler(httpd_req_t *req) { return serveFile(req, "/app.js", "application/javascript"); }

static bool startWebServer() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return false;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  if (httpd_start(&server, &config) != ESP_OK) {
    Serial.println("httpd_start failed");
    return false;
  }

  httpd_uri_t indexUri = { "/", HTTP_GET, indexHandler, NULL };
  httpd_uri_t cssUri   = { "/style.css", HTTP_GET, cssHandler, NULL };
  httpd_uri_t mixJsUri = { "/mix.js", HTTP_GET, mixJsHandler, NULL };
  httpd_uri_t appJsUri = { "/app.js", HTTP_GET, appJsHandler, NULL };

  httpd_uri_t wsUri;
  memset(&wsUri, 0, sizeof(wsUri));
  wsUri.uri          = "/ws";
  wsUri.method       = HTTP_GET;
  wsUri.handler      = wsHandler;
  wsUri.is_websocket = true;

  httpd_register_uri_handler(server, &indexUri);
  httpd_register_uri_handler(server, &cssUri);
  httpd_register_uri_handler(server, &mixJsUri);
  httpd_register_uri_handler(server, &appJsUri);
  httpd_register_uri_handler(server, &wsUri);

  return true;
}
```

- [ ] **Step 2: Manual verification note**

This file's WebSocket frame handling (`httpd_ws_recv_frame` called twice — once to size, once to fill) follows the standard ESP-IDF pattern, and this environment confirmed `CONFIG_HTTPD_WS_SUPPORT=y` is enabled in the installed esp32 core 3.3.11 (`~/.arduino15/packages/esp32/tools/esp32-libs/3.3.11/sdkconfig`), so `is_websocket`/`httpd_ws_*` are available without extra build config. The exact byte-count `ws_handler.h` assigns to `pkt.len` after the second `httpd_ws_recv_frame()` call was reasoned from ESP-IDF's documented convention, not confirmed by a live compile in this environment — if the first on-device test in Task 7 shows garbled/truncated drive messages, check this against the `esp_http_server` WebSocket example (`httpd_ws_recv_frame` in the ESP-IDF docs) first.

- [ ] **Step 3: Commit**

```bash
git add embedded/web_drive/ws_handler.h
git commit -m "Add web_drive ws_handler.h (WebSocket server, JSON, watchdog)"
```

---

## Task 3: `web_drive.ino` — setup/loop, Wi-Fi AP, mDNS

**Files:**
- Create: `embedded/web_drive/web_drive.ino`

**Interfaces:**
- Consumes: `driveSetup()` (Task 1), `startWebServer()`, `wsWatchdogCheck()`, `wsSendTelemetry()` (Task 2)
- Produces: nothing further (top of the dependency graph)

- [ ] **Step 1: Write `web_drive.ino`**

```cpp
/*
 * web_drive.ino - landscape browser drive control + telemetry, served
 * directly from the ESP32 over its own Wi-Fi AP.
 *
 * BRING-UP TOOL, not the real firmware: no SAFE_HOLD, no mode state machine
 * (spec.md §5). Two things stand in for that here:
 *   - a voltage-based duty clamp (drive_core.h, from drive_calib.ino) so
 *     3.0-6.0V motors never see more than ~5.5V from this 12V bus
 *   - a 400ms command watchdog (ws_handler.h): no valid "drive" message ->
 *     motors stop
 *
 * WHEELS OFF THE GROUND for first bring-up. Full design:
 * docs/superpowers/specs/2026-09-03-web-drive-gui-design.md
 *
 * Requires a LittleFS image built from this sketch's data/ folder
 * (index.html/style.css/mix.js/app.js) uploaded before first use - e.g.
 * Arduino IDE "ESP32 Sketch Data Upload" tool, or the equivalent for your
 * IDE version.
 */

#include <WiFi.h>
#include <ESPmDNS.h>

#include "drive_core.h"
#include "ws_handler.h"

static const char *AP_SSID     = "RobotDrive";
static const char *AP_PASSWORD = "drive1234";   // WPA2, 8+ chars - change before field use

static uint32_t telemetryNextMs = 0;
static const uint32_t TELEMETRY_MS = 200;   // ~5 Hz

void setup() {
  Serial.begin(115200);
  delay(300);

  driveSetup();   // kills both bridges first, then brings up pins + INA219s

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP started: ");
  Serial.println(WiFi.softAPIP());

  if (MDNS.begin("robotdrive")) {
    Serial.println("mDNS started: http://robotdrive.local/");
  } else {
    Serial.println("mDNS failed to start");
  }

  if (!startWebServer()) {
    Serial.println("Web server failed to start - halting");
    while (true) { delay(1000); }
  }
  Serial.println("Web server up. Open http://<AP IP>/ or http://robotdrive.local/");
}

void loop() {
  wsWatchdogCheck();

  if ((int32_t)(millis() - telemetryNextMs) >= 0) {
    telemetryNextMs = millis() + TELEMETRY_MS;
    wsSendTelemetry();
  }
}
```

- [ ] **Step 2: Compile check**

Run (no local ESP32 toolchain in this environment — do this in the Arduino IDE or with `arduino-cli` if installed on the machine that has the board attached):

```
arduino-cli compile --fqbn esp32:esp32:esp32 embedded/web_drive
```

Expected: compiles with no errors. If `arduino-cli`/the board package isn't set up, open `embedded/web_drive/web_drive.ino` in the Arduino IDE with board "ESP32 Dev Module" selected and use Sketch → Verify/Compile instead.

- [ ] **Step 3: Commit**

```bash
git add embedded/web_drive/web_drive.ino
git commit -m "Add web_drive.ino (Wi-Fi AP, mDNS, server wiring)"
```

---

## Task 4: `mix.js` — joystick-to-duty mixing (pure function, unit tested)

**Files:**
- Create: `embedded/web_drive/data/mix.js`
- Test: `embedded/web_drive/data/mix.test.js`

**Interfaces:**
- Consumes: nothing
- Produces: `mixDrive(dx, dy, speedCap)` → `{left, right}`, both integers in `[-255, 255]`. Global `mixDrive` when loaded via `<script>`; `module.exports.mixDrive` under Node.

- [ ] **Step 1: Write the failing test**

```js
// embedded/web_drive/data/mix.test.js
const assert = require("assert");
const { mixDrive } = require("./mix.js");

// straight forward, full stick, full speed
{
  const { left, right } = mixDrive(0, 1, 1);
  assert.strictEqual(left, 255);
  assert.strictEqual(right, 255);
}

// spin right in place, full stick, full speed (matches drive_test.ino's
// 'd' key: drive(speed, -speed))
{
  const { left, right } = mixDrive(1, 0, 1);
  assert.strictEqual(left, 255);
  assert.strictEqual(right, -255);
}

// spin left in place (matches drive_test.ino's 'a' key: drive(-speed, speed))
{
  const { left, right } = mixDrive(-1, 0, 1);
  assert.strictEqual(left, -255);
  assert.strictEqual(right, 255);
}

// speed cap scales the output
{
  const { left, right } = mixDrive(0, 1, 0.5);
  assert.strictEqual(left, 128);
  assert.strictEqual(right, 128);
}

// centered stick -> zero, regardless of speed cap
{
  const { left, right } = mixDrive(0, 0, 1);
  assert.strictEqual(left, 0);
  assert.strictEqual(right, 0);
}

// output never exceeds +/-255 even with out-of-range inputs
{
  const { left, right } = mixDrive(1, 1, 1);
  assert.ok(left <= 255 && left >= -255);
  assert.ok(right <= 255 && right >= -255);
}

console.log("mix.test.js: all assertions passed");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node embedded/web_drive/data/mix.test.js`
Expected: FAIL — `Error: Cannot find module './mix.js'` (file doesn't exist yet)

- [ ] **Step 3: Write `mix.js`**

```js
// mix.js - pure joystick -> differential-drive duty mixing.
// dx, dy in [-1,1] (dx: left/right, dy: forward/back). speedCap in [0,1].
// Returns {left, right}, each an integer in [-255,255]. Loadable both as a
// plain <script> (defines a global) and as a Node module (for mix.test.js).
function mixDrive(dx, dy, speedCap) {
  const cap = Math.max(0, Math.min(1, speedCap));
  const clamp = v => Math.max(-255, Math.min(255, v));
  const left  = clamp(Math.round((dy + dx) * 255 * cap));
  const right = clamp(Math.round((dy - dx) * 255 * cap));
  return { left, right };
}

if (typeof module !== "undefined") module.exports = { mixDrive };
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node embedded/web_drive/data/mix.test.js`
Expected: PASS — prints `mix.test.js: all assertions passed`

- [ ] **Step 5: Commit**

```bash
git add embedded/web_drive/data/mix.js embedded/web_drive/data/mix.test.js
git commit -m "Add mix.js joystick-to-duty mixing with unit test"
```

---

## Task 5: `index.html` + `style.css` — HUD layout and styling

**Files:**
- Create: `embedded/web_drive/data/index.html`
- Create: `embedded/web_drive/data/style.css`

**Interfaces:**
- Consumes: nothing
- Produces: DOM element ids that Task 6's `app.js` binds to: `statusRing`, `joystickBase`, `joystickPuck`, `speedCap`, `speedCapValue`, `warningLabel`, `currentLeftArc`, `currentRightArc`, `vbusValue`, `dutyValue`, `batteryFill`, `batteryLabel`. SVG arc elements use `r="80"` (left) and `r="60"` (right), matched by `app.js`'s circumference math.

- [ ] **Step 1: Write `index.html`**

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>Robot Drive</title>
<link rel="stylesheet" href="/style.css">
</head>
<body>
  <div class="hud">
    <section class="panel panel-control">
      <div class="status-ring" id="statusRing">
        <div class="joystick-base" id="joystickBase">
          <div class="joystick-puck" id="joystickPuck"></div>
        </div>
      </div>
      <div class="speed-cap">
        <label for="speedCap">SPEED CAP</label>
        <input type="range" id="speedCap" min="0" max="100" value="60">
        <span id="speedCapValue">60%</span>
      </div>
      <div class="warning" id="warningLabel"></div>
    </section>

    <section class="panel panel-telemetry">
      <svg viewBox="0 0 200 200" class="telemetry-dial">
        <circle cx="100" cy="100" r="90" class="dial-ring"/>
        <circle cx="100" cy="100" r="70" class="dial-ring-inner"/>
        <circle id="currentLeftArc" cx="100" cy="100" r="80" class="arc arc-left"/>
        <circle id="currentRightArc" cx="100" cy="100" r="60" class="arc arc-right"/>
        <text x="100" y="90" class="dial-label" text-anchor="middle">VBUS</text>
        <text id="vbusValue" x="100" y="112" class="dial-value" text-anchor="middle">--.- V</text>
        <text id="dutyValue" x="100" y="132" class="dial-sub" text-anchor="middle">L -- / R --</text>
      </svg>
      <div class="battery-gauge">
        <div class="battery-fill" id="batteryFill"></div>
        <span id="batteryLabel">-- V</span>
      </div>
    </section>
  </div>

  <script src="/mix.js"></script>
  <script src="/app.js"></script>
</body>
</html>
```

- [ ] **Step 2: Write `style.css`**

```css
:root {
  --bg: #04080a;
  --teal: #2de2c8;
  --teal-dim: rgba(45, 226, 200, 0.25);
  --orange: #ff8a3d;
  --red: #ff4d4d;
  --panel-bg: rgba(10, 24, 26, 0.6);
  --text: #d9fff6;
}

* { box-sizing: border-box; }

html, body {
  margin: 0;
  width: 100%;
  height: 100%;
  background: var(--bg);
  color: var(--text);
  font-family: "Segoe UI", system-ui, sans-serif;
  overflow: hidden;
  touch-action: none;
}

.hud {
  display: flex;
  flex-direction: row;
  align-items: center;
  justify-content: space-around;
  width: 100vw;
  height: 100vh;
  padding: 16px;
}

.panel {
  background: var(--panel-bg);
  border: 1px solid var(--teal-dim);
  border-radius: 12px;
  padding: 16px;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 12px;
  min-width: 40vw;
  height: 90vh;
  justify-content: center;
}

/* ---- control panel ---- */
.status-ring {
  width: min(40vh, 40vw);
  height: min(40vh, 40vw);
  border-radius: 50%;
  border: 3px solid var(--teal-dim);
  display: flex;
  align-items: center;
  justify-content: center;
  transition: border-color 200ms, box-shadow 200ms;
}

.status-ring.status-connected {
  border-color: var(--teal);
  box-shadow: 0 0 24px var(--teal-dim);
}

.status-ring.status-disconnected,
.status-ring.status-stopped {
  border-color: var(--red);
  box-shadow: 0 0 24px rgba(255, 77, 77, 0.35);
  animation: pulse 1s ease-in-out infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50%      { opacity: 0.5; }
}

.joystick-base {
  position: relative;
  width: 70%;
  height: 70%;
  border-radius: 50%;
  background: radial-gradient(circle, rgba(255,138,61,0.15), transparent 70%);
  border: 2px solid var(--orange);
  touch-action: none;
}

.joystick-puck {
  position: absolute;
  top: 50%;
  left: 50%;
  width: 28%;
  height: 28%;
  margin: -14% 0 0 -14%;
  border-radius: 50%;
  background: var(--teal);
  box-shadow: 0 0 16px var(--teal);
  pointer-events: none;
}

.speed-cap {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.8rem;
  letter-spacing: 0.1em;
}

.warning {
  min-height: 1.2em;
  color: var(--red);
  font-weight: bold;
  letter-spacing: 0.1em;
}

/* ---- telemetry panel ---- */
.telemetry-dial {
  width: min(50vh, 40vw);
  height: min(50vh, 40vw);
}

.dial-ring        { fill: none; stroke: var(--teal-dim); stroke-width: 2; }
.dial-ring-inner  { fill: none; stroke: var(--teal-dim); stroke-width: 1; }

.arc {
  fill: none;
  stroke-width: 6;
  stroke-linecap: round;
  transform: rotate(-90deg);
  transform-origin: 100px 100px;
  transition: stroke-dashoffset 200ms linear;
}
.arc-left  { stroke: var(--teal); }
.arc-right { stroke: var(--orange); }

.dial-label { fill: var(--teal-dim); font-size: 10px; letter-spacing: 0.2em; }
.dial-value { fill: var(--text); font-size: 18px; }
.dial-sub   { fill: var(--teal-dim); font-size: 10px; }

.battery-gauge {
  width: 24px;
  height: 120px;
  border: 2px solid var(--red);
  border-radius: 4px;
  display: flex;
  flex-direction: column-reverse;
  overflow: hidden;
  position: relative;
}

.battery-fill {
  background: linear-gradient(to top, var(--red), var(--orange));
  height: 0%;
  transition: height 300ms ease;
}

#batteryLabel {
  position: absolute;
  bottom: -20px;
  left: 50%;
  transform: translateX(-50%);
  font-size: 0.7rem;
  white-space: nowrap;
}

@media (max-width: 700px) {
  .hud { flex-direction: column; }
  .panel { width: 90vw; min-width: unset; height: 45vh; }
}
```

- [ ] **Step 3: Syntax check**

Run: `node --check embedded/web_drive/data/index.html 2>&1 || true` is not applicable to HTML — instead visually open `index.html` directly in a desktop browser (`file://` path) to confirm the two panels render without console errors (the `mixDrive`/`WebSocket` calls in `app.js` don't exist yet until Task 6, so expect a JS error in the console at this stage — the layout/CSS itself should still render).

- [ ] **Step 4: Commit**

```bash
git add embedded/web_drive/data/index.html embedded/web_drive/data/style.css
git commit -m "Add web_drive HUD layout and styling"
```

---

## Task 6: `app.js` — WebSocket client, joystick input, telemetry rendering

**Files:**
- Create: `embedded/web_drive/data/app.js`

**Interfaces:**
- Consumes: `mixDrive(dx, dy, speedCap)` (Task 4, loaded as a global via `<script src="/mix.js">` before this file); DOM ids from Task 5
- Produces: nothing further (top of the frontend dependency graph)

- [ ] **Step 1: Write `app.js`**

```js
// app.js - WebSocket client, joystick input, telemetry rendering.
// Depends on mixDrive() from mix.js (loaded before this file in index.html).

(function () {
  const WS_URL = "ws://" + location.host + "/ws";
  const KEEPALIVE_MS = 150;   // well under the ESP32's 400ms watchdog

  const statusRing   = document.getElementById("statusRing");
  const joystickBase = document.getElementById("joystickBase");
  const joystickPuck = document.getElementById("joystickPuck");
  const speedCap      = document.getElementById("speedCap");
  const speedCapValue = document.getElementById("speedCapValue");
  const warningLabel  = document.getElementById("warningLabel");

  const currentLeftArc  = document.getElementById("currentLeftArc");
  const currentRightArc = document.getElementById("currentRightArc");
  const vbusValue = document.getElementById("vbusValue");
  const dutyValue = document.getElementById("dutyValue");
  const batteryFill  = document.getElementById("batteryFill");
  const batteryLabel = document.getElementById("batteryLabel");

  let ws = null;
  let connected = false;
  let dx = 0, dy = 0;          // current stick position, [-1,1]
  let dragging = false;
  let keepaliveTimer = null;

  function setStatus(state) {
    // state: "connected" | "disconnected" | "stopped"
    statusRing.classList.remove("status-connected", "status-disconnected", "status-stopped");
    statusRing.classList.add("status-" + state);
  }

  function setWarning(text) {
    warningLabel.textContent = text || "";
  }

  function sendDrive() {
    if (!connected || !ws || ws.readyState !== WebSocket.OPEN) return;
    const cap = speedCap.value / 100;
    const { left, right } = mixDrive(dx, dy, cap);
    ws.send(JSON.stringify({ type: "drive", left, right }));
  }

  function connect() {
    ws = new WebSocket(WS_URL);

    ws.onopen = () => {
      connected = true;
      setStatus("connected");
      setWarning("");
    };

    ws.onclose = () => {
      connected = false;
      setStatus("disconnected");
      setWarning("LINK LOST");
      stopKeepalive();
      setTimeout(connect, 1000);   // keep trying to reconnect
    };

    ws.onerror = () => {
      ws.close();
    };

    ws.onmessage = (evt) => {
      let msg;
      try { msg = JSON.parse(evt.data); } catch (e) { return; }
      if (msg.type === "telemetry") renderTelemetry(msg);
    };
  }

  function renderTelemetry(msg) {
    const leftPct  = Math.max(0, Math.min(1, msg.left_a  / 2));
    const rightPct = Math.max(0, Math.min(1, msg.right_a / 2));
    setArc(currentLeftArc,  80, leftPct);
    setArc(currentRightArc, 60, rightPct);

    vbusValue.textContent = msg.vbus.toFixed(1) + " V";
    dutyValue.textContent = "L " + msg.left_duty + " / R " + msg.right_duty;

    const battPct = Math.max(0, Math.min(1, (msg.vbus - 9.0) / (12.6 - 9.0)));
    batteryFill.style.height = (battPct * 100).toFixed(0) + "%";
    batteryLabel.textContent = msg.vbus.toFixed(1) + " V";
  }

  function setArc(circleEl, radius, pct) {
    const circumference = 2 * Math.PI * radius;
    circleEl.style.strokeDasharray = circumference.toFixed(1);
    circleEl.style.strokeDashoffset = (circumference * (1 - pct)).toFixed(1);
  }

  // ---- joystick input ----
  function stickPosFromEvent(evt) {
    const rect = joystickBase.getBoundingClientRect();
    const cx = rect.left + rect.width / 2;
    const cy = rect.top + rect.height / 2;
    const r  = rect.width / 2;
    let nx = (evt.clientX - cx) / r;
    let ny = (evt.clientY - cy) / r;
    const mag = Math.hypot(nx, ny);
    if (mag > 1) { nx /= mag; ny /= mag; }
    return { nx, ny };
  }

  function updatePuck(nx, ny) {
    const r = joystickBase.clientWidth / 2;
    joystickPuck.style.transform =
      "translate(" + (nx * r) + "px," + (ny * r) + "px)";
  }

  function startKeepalive() {
    stopKeepalive();
    keepaliveTimer = setInterval(sendDrive, KEEPALIVE_MS);
  }

  function stopKeepalive() {
    if (keepaliveTimer) { clearInterval(keepaliveTimer); keepaliveTimer = null; }
  }

  function onPointerDown(evt) {
    dragging = true;
    joystickBase.setPointerCapture(evt.pointerId);
    onPointerMove(evt);
    startKeepalive();
  }

  function onPointerMove(evt) {
    if (!dragging) return;
    const { nx, ny } = stickPosFromEvent(evt);
    dx = nx;
    dy = -ny;   // screen Y is down-positive; forward should be up
    updatePuck(nx, ny);
    sendDrive();
  }

  function onPointerUp() {
    dragging = false;
    dx = 0; dy = 0;
    updatePuck(0, 0);
    sendDrive();
    stopKeepalive();
  }

  joystickBase.addEventListener("pointerdown", onPointerDown);
  joystickBase.addEventListener("pointermove", onPointerMove);
  joystickBase.addEventListener("pointerup", onPointerUp);
  joystickBase.addEventListener("pointercancel", onPointerUp);

  speedCap.addEventListener("input", () => {
    speedCapValue.textContent = speedCap.value + "%";
  });

  setStatus("disconnected");
  connect();
})();
```

- [ ] **Step 2: Syntax check**

Run: `node --check embedded/web_drive/data/app.js`
Expected: no output (exit code 0) — confirms valid JS syntax. This does not exercise DOM/WebSocket behavior (Node has neither); that's covered by Task 7's on-device test.

- [ ] **Step 3: Commit**

```bash
git add embedded/web_drive/data/app.js
git commit -m "Add web_drive app.js (WebSocket client, joystick, telemetry UI)"
```

---

## Task 7: End-to-end bring-up and verification

**Files:** none created — this task flashes and exercises the sketch built by Tasks 1-6.

**Interfaces:**
- Consumes: everything from Tasks 1-6
- Produces: a verified, working bring-up tool

- [ ] **Step 1: Upload the LittleFS data image**

In the Arduino IDE with `embedded/web_drive` open and board "ESP32 Dev Module" selected: Tools → "ESP32 Sketch Data Upload" (or the equivalent LittleFS upload tool for your IDE version) to flash `data/index.html`, `data/style.css`, `data/mix.js`, `data/app.js` to the board's LittleFS partition.

- [ ] **Step 2: Upload the sketch**

Sketch → Upload for `web_drive.ino`. Open Serial Monitor at 115200 baud and confirm: `AP started: <ip>`, `mDNS started: http://robotdrive.local/`, `Web server up. ...`.

- [ ] **Step 3: Wheels off the ground — connect and confirm the page loads**

Join the `RobotDrive` Wi-Fi network from a phone/laptop, browse to `http://robotdrive.local/` (or the printed AP IP). Confirm the landscape HUD renders: joystick panel left, telemetry panel right, status ring shows the "connected" (teal glow) state within ~1s of page load.

- [ ] **Step 4: Confirm each drive direction**

With wheels off the ground, drag the joystick in each of the four cardinal directions and diagonals. Confirm motor spin direction matches the joystick direction (forward = both sides spin the "forward" way, left/right on the stick spins the robot the corresponding way) — cross-check against `drive_test.ino`'s `w/a/s/d` behavior on the same hardware if unsure which physical direction is "forward" for this chassis.

- [ ] **Step 5: Confirm the speed cap**

Move the speed-cap slider to ~30% and confirm max joystick deflection now produces visibly slower motor speed than at 100%.

- [ ] **Step 6: Confirm the watchdog**

While holding the joystick in a driving position, either turn off Wi-Fi on the client device or close the browser tab. Confirm (via Serial Monitor and/or listening/observing the motors) that motors reach zero within roughly 400-1000ms (400ms watchdog + reasonable margin for detection latency).

- [ ] **Step 7: Cross-check telemetry against `drive_test.ino`**

Flash `drive_test.ino` temporarily (or use a second, already-programmed board if available), drive at a known duty, and read current via its `c` command. Re-flash `web_drive.ino`, drive at a comparable duty via the GUI, and confirm the telemetry panel's current readout is in the same ballpark (not necessarily identical, since load/friction vary run to run).

- [ ] **Step 8: Confirm the duty clamp**

With the defaults (`vBusManual=12.6V`, `v0Drop=1.9V`, `rTot=2.5Ω`, `V_MOTOR_MAX=5.5V`), `dutyMaxCounts()` should compute to `floor(5.5 / (12.6 - 1.9) * 255) = 129`. Confirm via Serial logging (temporarily add a `Serial.println(dutyMaxCounts())` in `setup()` if not already visible) that commanded duty never exceeds this value even at full joystick deflection and 100% speed cap.

- [ ] **Step 9: Commit any fixes found during bring-up**

If any of steps 3-8 required code changes (e.g. inverted direction, wrong pin, watchdog timing), commit them with a message describing what bring-up revealed — this is expected; hardware bring-up finding issues the design/code review didn't catch is normal, not a plan failure.

```bash
git add embedded/web_drive/
git commit -m "Fix issues found during web_drive bring-up: <describe>"
```

---

## Self-Review Notes

**Spec coverage**: design doc §§2-9 all map to tasks above — D1/D4 (Task 1, new sketch not modifying `drive_test.ino`), D2 (Task 2, WebSocket), D3 (Task 2, watchdog), D5 (Task 1, duty clamp), D6 (Tasks 4-6, vanilla JS/no build step), D7 (Task 5, landscape two-panel layout), §4 message protocol (Tasks 2 and 6, matched shapes), §6 visual design (Task 5 layout + Task 6 rendering), §8 testing plan (Task 7).

**Placeholder scan**: no TBD/TODO in code; the two open items from the design doc's own §9 (AP SSID/password, watchdog timeout value) were resolved to concrete values here (`RobotDrive`/`drive1234`, `400ms`) rather than left open, since a plan can't ship a placeholder — flagged in Global Constraints as `[ASSUMPTION]`-equivalent values a bring-up session can freely change.

**Type/name consistency checked**: `drive()`, `stopAll()`, `readCurrents()`, `readBusVoltage()`, `dutyMaxCounts()`, `isense[]`, `lastDuty[]`, `LEFT`/`RIGHT` used identically across Tasks 1-3; `mixDrive()` signature identical across Tasks 4 and 6; DOM ids identical across Tasks 5 and 6; WebSocket message field names (`left`, `right`, `left_a`, `right_a`, `left_duty`, `right_duty`, `vbus`) identical across Tasks 2 and 6.
