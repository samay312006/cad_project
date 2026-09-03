# Design — Web-Based Drive & Telemetry GUI (Bring-up Tool)

Status: design pending user review, not yet implemented.
Owner: cloud/GUI engineer (this session). See §0 — this touches ESP32-side code,
outside the normal cloud/GUI boundary, under explicit user direction for this tool.
Implements: an ad hoc bring-up tool, not a `spec.md` FR. Precursor in spirit to
`spec.md` §6.1 (ESP32↔Phone contract) and step 5 of `plan.md` 2c.3/2c.4 (bring
up drive before anything else). Does not supersede or modify `drive_test.ino`,
which remains the Serial-tethered bench tool.

---

## 0. Role-boundary note

`ONBOARDING.md` assigns ESP32 firmware to the embedded engineer and this session
is configured (`CLAUDE.local.md`) as cloud/GUI. This design's ESP32-side half
(Wi-Fi, HTTP/WebSocket server, LittleFS serving) is firmware work outside that
boundary. The user explicitly directed building it end-to-end this session,
consistent with how the ESP32-CAM streaming sketch was handled earlier — noted
here rather than silently crossed. No change to `spec.md`'s interface contracts
or to the embedded engineer's owned files.

---

## 1. Scope

A browser-based joystick + speed-cap drive control and live current/voltage
telemetry display, served directly from the ESP32 over its own Wi-Fi AP. This
is a **bring-up/demo tool**, not the project's final control architecture —
it's expected to be superseded once the real Android app + full ESP32 firmware
(mode state machine, `SAFE_HOLD`, `spec.md` §6.1 protocol) exists.

**In scope (v1)**: directional drive (joystick), speed cap, live per-side motor
current + bus voltage telemetry, a command watchdog for safety.

**Out of scope**: autonomous/follow-me modes, electromagnet control, IMU/GPS
telemetry, `SAFE_HOLD`/mode state machine, MQTT/cloud — anything beyond direct
manual drive and the telemetry needed to drive safely.

---

## 2. Decisions

| # | Decision | Rejected alternative |
|---|---|---|
| D1 | New sketch `embedded/web_drive/web_drive.ino` | Modifying `drive_test.ino` directly — its own header says it "stays untouched," same principle `drive_calib.ino` already follows |
| D2 | WebSocket (`esp_http_server`'s `httpd_ws_*`) for both commands and telemetry | HTTP polling (too much per-request latency for a joystick); raw UDP (would need hand-rolled reconnect/framing for no real benefit here — the camera's earlier TCP stalls were about bulk JPEG frame data, not small JSON control messages, so that risk doesn't carry over the same way) |
| D3 | Command watchdog: no `drive` message for `[ASSUMPTION] 400ms` → force `drive(0,0)`; also stop immediately on WS disconnect | Latching like `drive_test.ino` (safe there because a human's thumb is on the keyboard; unsafe here because a dropped Wi-Fi command would leave the robot driving blind) |
| D4 | Copy (not share) `drive_test.ino`'s pin map, `Side`/`CurrentSensor` structs, `setSide()`/`drive()`/`readCurrents()` | A shared header/library — adds build complexity disproportionate to a bring-up tool; revisit only if a third sketch needs the same code |
| D5 | Apply `drive_calib.ino`'s voltage-based duty clamp (`dutyMaxCounts()`, `V_MOTOR_MAX = 5.5V`) using its current default constants (`v0Drop=1.9V`, `rTot=2.5Ω`, `vBusManual=12.6V`) | Inheriting `drive_test.ino`'s **uncapped** duty — flagged as a live, unmitigated risk in `docs/superpowers/specs/2026-09-03-incline-compensation-design.md` §3.1 ("not prevented by anything currently in the firmware"); no reason for a new sketch to reintroduce a known hazard |
| D6 | LittleFS-served static frontend (`index.html`/`style.css`/`app.js`), vanilla JS/CSS/SVG, no framework or build step | React/Vue/etc. (unjustified flash + build overhead for ESP32-hosted assets); inline C-string HTML like the camera's index page (unwieldy at this size, forces a full reflash per text edit) |
| D7 | Landscape, two-panel layout: left = joystick + connection/watchdog status ring, right = telemetry dial + battery gauge | Portrait single-column (the reference image's native layout) — rejected per user's explicit direction to split control/telemetry left/right |

---

## 3. Architecture

```
 Browser (phone/laptop, on the ESP32's AP)
    │  WebSocket /ws (JSON)          GET /, /style.css, /app.js
    ▼                                        ▼
 ESP32 — esp_http_server
    │
    ├─ ws_handler(): parses {"type":"drive",...} → setSide()/drive()
    │                pushes {"type":"telemetry",...} ~5 Hz from readCurrents()
    │                resets a watchdog deadline on every valid drive message
    │
    ├─ watchdog task/check: deadline expired → drive(0,0), mark disconnected
    │
    ├─ static file handlers → LittleFS (index.html / style.css / app.js)
    │
    └─ drive core (copied from drive_test.ino):
         pin map (ENA/IN1/IN2/ENB/IN3/IN4) → L298N → 4 motors (2 per side, paralleled)
         duty clamp (from drive_calib.ino) → setSide() → pwmWrite()
         INA219 ×2 (I2C, 0x40/0x41) → readCurrents()
```

No change to the physical wiring documented in `drive_test.ino`/`drive_calib.ino` —
this is a new software entry point onto hardware that already works.

---

## 4. Message protocol

Browser → ESP32, sent on joystick/slider movement (and/or a low-rate keepalive
while held at a fixed position, so the watchdog doesn't trip mid-hold):

```json
{"type": "drive", "left": -255, "right": 255}
```

`left`/`right` are pre-mixed by the frontend (joystick position → skid-steer
duty per side) so the ESP32 side stays dumb — same shape `drive_test.ino`'s
`drive(left, right)` already takes.

ESP32 → Browser, ~`[ASSUMPTION] 5 Hz`:

```json
{"type": "telemetry", "left_a": 0.42, "right_a": 0.39, "left_duty": 180, "right_duty": 178, "vbus": 12.1}
```

Straight from `readCurrents()` (amps) and `lastDuty[]`/`readBusVoltage()`-style
values.

---

## 5. Safety

- **Duty clamp (D5)**: every `pwmWrite()` call is capped by `dutyMaxCounts()`
  (voltage-based, from `drive_calib.ino`) before it reaches the motors — this
  is the one thing `drive_test.ino` currently lacks and that has been flagged
  as a real hazard given the 12V-bus/3–6V-motor mismatch.
- **Command watchdog (D3)**: `400ms [ASSUMPTION]` since the last valid `drive`
  message → force stop. Also stop immediately on WebSocket close/error.
- **Boot-safe default**: motors start at zero duty on boot, same as
  `drive_test.ino`/`drive_calib.ino` already do (`stopAll()` before anything
  else in `setup()`).
- Not implemented here, deliberately: `spec.md`'s full `SAFE_HOLD` interlock,
  overcurrent trip, or stall detection — this is a bring-up tool, not the real
  firmware. The duty clamp and watchdog are the minimum bar consistent with
  the project's stated fail-safe-default principle
  (`overview_controls.md` §5), not a substitute for the eventual real design.

---

## 6. Visual / UI design

Landscape orientation, two panels, built from the Iron-Man-HUD-style reference
image the user provided:

**Left panel — drive control.** The reference's orange-pincer/teal-center-circle
element becomes the joystick: drag within it to command left/right duty. The
teal arc brackets already wrapping that shape become a connection/watchdog
status ring around the stick — solid glow = WS connected & watchdog armed, red
pulse = disconnected or watchdog-stopped. A speed-cap slider sits near the
joystick.

**Right panel — telemetry.** The reference's globe/degree-ring dial moves here,
world-map texture dropped: left/right motor current rendered as two concentric
arc-fill rings inside the same circular frame, `vbus`/duty as a numeric readout
in the dial's center. The reference's bottom-left segmented red battery gauge
folds into this same panel (under/beside the dial) rather than sitting
separately, so all telemetry lives in one place.

**Between/under the panels**: a warning-label slot (styled after the reference's
"Temp Warning" label) — blank normally, shows `LINK LOST` / `WATCHDOG STOP`
text when triggered.

**Explicitly deferred** (decorative, not blocking v1): the reference's ring
rotation animation, textured/animated globe, and fine tick-mark/glow
micro-detail. These are polish passes for later, consistent with the user's
"keep updating the GUI as we go" plan — v1 gets the functional layout and the
core color/glow language (dark ground, cyan/teal primary, orange/red accents),
not full visual parity.

---

## 7. File layout

```
embedded/web_drive/
  web_drive.ino
  data/
    index.html
    style.css
    app.js
```

`data/` is the Arduino LittleFS-upload tool's convention (mirrors how the
ESP32-CAM project would add a filesystem image, though that project doesn't
use one yet).

---

## 8. Testing plan

1. Wheels off the ground (same rule `drive_test.ino` already enforces in its
   own header).
2. Confirm each drive direction and the speed-cap slider behave as commanded.
3. Confirm the watchdog: kill Wi-Fi or close the browser tab, confirm motors
   reach zero within timeout.
4. Cross-check telemetry current readings against `drive_test.ino`'s
   known-good `c` command output on the same hardware.
5. Confirm the duty clamp actually caps duty at the expected value for the
   current `v0Drop`/`rTot`/`vBusManual` defaults.

---

## 9. Open assumptions / TBD

- Watchdog timeout `400ms` — untested against this board's real WebSocket
  latency; may need tuning once on hardware (`[VERIFY]`).
- `v0Drop`/`rTot` are `drive_calib.ino`'s **defaults**, not a real measured
  calibration yet (no `embedded/control/calibration.h` exists in the repo).
  The duty clamp should be re-derived once real calibration data exists — this
  design doesn't block on that, but shouldn't be mistaken for a validated cap.
- AP SSID/password and mDNS hostname not yet chosen — `[TBD]`, pick at
  implementation time (or ask the user), same pattern as `esp32cam`/`Shini`
  earlier this session.
- Exact joystick interaction mechanics (drag-anywhere-within-circle vs.
  fixed-origin stick-and-return) left to implementation detail.
