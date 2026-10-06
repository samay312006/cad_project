# Cloud Web Dashboard (ESP32 ↔ EC2 Direct) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the local-AP `embedded/web_drive/` bring-up tool (drive + telemetry) to the approved ESP32↔EC2-direct-MQTT architecture, running entirely against a local Mosquitto rehearsal stack — no AWS resource is created by this plan — and bring every project document that described the old ESP32↔Phone↔EC2 two-hop design into sync with the new one-hop design.

**Architecture:** ESP32 drops its own Wi-Fi AP and becomes a Wi-Fi *station* + MQTT client, publishing `robot/{id}/telemetry` and subscribing to `robot/{id}/drive` on a Mosquitto broker. The browser dashboard (ported from `embedded/web_drive/data/`) becomes an MQTT-over-WebSocket client of the same broker — it is no longer a bridge sitting between the ESP32 and the cloud, just another client of the broker. `drive_core.h` (motor drive + duty clamp) is reused unmodified. Everything in this plan is validated against a local Dockerized Mosquitto broker (`cloud/docker-compose.yml`); pointing at a real EC2 broker is explicitly out of scope (requires the human's separate go-ahead per `CLAUDE.md`, and is already covered as a configuration-only swap by `plan.md` 2c.1).

**Tech Stack:** ESP32 Arduino core (`WiFi.h`, `PubSubClient` for MQTT), Mosquitto 2.x (Docker), nginx (static file serving, Docker locally / real host later), MQTT.js (browser, loaded via CDN script tag — no bundler in this repo), plain Node.js `assert`-based tests (matches the existing `mix.test.js` convention — no new test framework or npm dependency introduced).

**Spec:** `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md` (approved by the user 2026-10-04 — all of D1–D7). This plan also carries out the doc-sync edits that design doc deferred to "the implementation plan" (its §0 and final bullet of §9).

## Role-boundary note (read before executing)

This plan's cloud-side tasks (1, 2, 3, 5, 6, 7) are squarely cloud/GUI-engineer territory. **Task 4 writes ESP32 firmware** (`embedded/web_drive/web_drive.ino`, `embedded/web_drive/mqtt_handler.h`) — normally out of bounds for the cloud/GUI role per `CLAUDE.local.md`/`ONBOARDING.md`. This is being done as an explicit, one-off exception, same as the original `embedded/web_drive/` bring-up tool and confirmed again by the human specifically for this task on 2026-10-04 (see `UPDATES.md` blocker #8's resolution, Task 7 of this plan). Do not treat this as a standing precedent for future ESP32 work — each exception needs its own explicit go-ahead.

## Scope boundary (what this plan deliberately does NOT touch)

- **No AWS resource is created.** `plan.md` §2b (AWS setup) and the EC2 bring-up checklist in the design doc §6 remain written-but-not-executed, per `CLAUDE.md`'s standing rule. This plan's local Mosquitto stack (`cloud/docker-compose.yml`) is the full extent of the "broker" work here.
- **No physical hardware bring-up.** The design doc's §8 testing-plan items 2–6 (wheels off the ground, direction/speed checks, watchdog-trip checks on a real link, telemetry cross-check against `drive_test.ino`, repeat against real EC2) need a human at a real ESP32 bench. This plan ends at "local rehearsal stack + firmware source ready to flash" — Task 7 puts the physical bring-up on `UPDATES.md` as a new, explicit next step, same pattern as the original `embedded/web_drive/` blocker (`UPDATES.md` #7).
- **`plan.md` §2a's local-sim architecture (the Gazebo/ROS2 "simulated-phone" node, §2a.2/2a.3/2a.5/2a.6) is NOT updated by this plan.** It still describes the old ESP32↔phone JSON-over-UDP contract. That's a materially separate, larger body of simulation work (SLAM/Nav2, not yet built) and wasn't in the doc-sweep scope the human approved (spec.md full sweep + `overview_cloud.md`/`overview_controls.md` + `plan.md` §2c only). Task 7 flags this remaining inconsistency in `UPDATES.md` rather than silently leaving it undocumented.
- **The scan/odom/map/waypoints/cmd/mode MQTT topics are not implemented.** Only `robot/{id}/drive` and `robot/{id}/telemetry` exist after this plan, matching the design doc's v1 scope (D7). The doc edits in Task 5 say so explicitly rather than implying those topics are live.

## Global Constraints

- Never create a real AWS resource, spend money, or run a cloud command without the human's explicit confirmation first (`CLAUDE.md`, `CLAUDE.local.md`) — nothing in this plan does, but don't let later follow-up work skip this.
- Local-first, always — every piece of this plan is validated against the local Mosquitto/Docker stack before anything points at a real broker.
- `spec.md` §6 is a shared interface contract — Task 5's edits there get a corresponding `UPDATES.md` row (Task 7) so the embedded engineer's session sees the change, per `CLAUDE.md`'s "no unilateral shared-schema change" rule.
- Command watchdog timeout is `1500ms` (design D6) — not `web_drive`'s old `400ms` local-AP value. This shows up in `embedded/web_drive/mqtt_handler.h` (Task 4) and in every doc reference to it (Task 5/6).
- MQTT topics are exactly `robot/{id}/drive` and `robot/{id}/telemetry`, `{id}` fixed to `"robot1"` for now (design §4, D-doc `[ASSUMPTION]`) — this id must match between `embedded/web_drive/mqtt_handler.h` (`ROBOT_ID`) and `cloud/dashboard/mqtt_topics.js` (`ROBOT_ID`). It's a plain string literal duplicated in two languages, not a shared constants file — keep it in sync by hand, there's no build step to generate one from the other.
- No new npm dependency is introduced (this repo has no `package.json`/bundler; `mix.test.js` already runs via plain `node file.test.js` + the built-in `assert` module — Task 2's test follows the identical pattern). The browser's MQTT client comes from a CDN `<script>` tag, not an npm install.
- `drive_core.h` is unchanged by this plan (per design doc §7 file-layout table) — do not edit pin map, duty-clamp constants, or drive logic in it.

## Review Focus

- **Malformed/oversized/non-JSON payload on `robot/{id}/drive`** (garbage bytes, a huge length, a well-formed-but-out-of-range `left`/`right`) — firmware must drop it silently without resetting the watchdog and without crashing or leaving unread bytes that desync a later message. No automated test exists for this in C++ in this repo (same as the predecessor `ws_handler.h` — ESP32 firmware isn't unit-tested here); pinned instead by an explicit bench-check item added to Task 4's verification checklist.
- **Broker unreachable at ESP32 boot or mid-session** — motors must not keep coasting at a stale duty while `PubSubClient::connect()` blocks (it can block for several seconds against an unreachable host, long enough to blow past the 1500ms watchdog window on its own). Task 4's `mqttReconnect()` calls `stopAll()` *before* the blocking connect attempt specifically to close this gap; its bench-check step verifies it.
- **Dashboard receives a telemetry payload with a missing field, wrong type, or isn't valid JSON at all** — must be dropped, never rendered as `NaN`/`undefined` in the UI. Covered by Task 2's `parseTelemetryMessage` unit tests (malformed JSON, missing-field, non-object cases).
- **Dashboard loses its MQTT connection while the joystick is actively held down** — must stop publishing (not queue-and-flush once reconnected) and must visibly show "LINK LOST," not silently freeze the last-sent command. Covered by Task 3's `connected`-gated `sendDrive()` plus an explicit manual browser-check step (kill the broker mid-drag, confirm the UI reflects it and the keepalive timer stops).
- **Multiple dashboard clients (two browser tabs, two devices) publishing to `robot/{id}/drive` at once** — the old local-AP design enforced at most one WebSocket client (`wsClientFd` was a single slot); this MQTT redesign has **no equivalent arbitration** — last publish simply wins, with no detection that a second controller exists. This is a genuine new gap versus the predecessor design, not something this plan fixes (out of v1 scope per design D7), but it's flagged explicitly in Task 7's `UPDATES.md` note rather than silently dropped.

---

### Task 1: Local Mosquitto broker + rehearsal stack

**Files:**
- Create: `cloud/mosquitto/mosquitto.conf`
- Create: `cloud/docker-compose.yml`

**Interfaces:**
- Produces: a broker reachable at `localhost:1883` (plain MQTT, for the ESP32/CLI tools) and `localhost:9001` (MQTT-over-WebSocket, for the browser dashboard) once `docker compose up -d` is run from `cloud/`. Task 2/3/4 all connect to these.

- [ ] **Step 1: Write the broker config**

Create `cloud/mosquitto/mosquitto.conf`:

```
# Local dev broker for the cloud-hosted drive dashboard (design doc:
# docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md).
#
# Matches plan.md 2a.1's local-dev convention: open/no-TLS for local
# convenience (localhost/docker-network only, no real exposure). The EC2
# broker MUST NOT reuse this file - it needs TLS + username/password auth
# per plan.md 2b.3/2b.1 and design decisions D4/D5. This file is for the
# local rehearsal step only (design doc §8 item 1).

listener 1883
allow_anonymous true

# MQTT-over-WebSocket - what the browser dashboard (cloud/dashboard/app.js)
# actually speaks, since browsers can't open a raw TCP MQTT socket.
listener 9001
protocol websockets
allow_anonymous true
```

- [ ] **Step 2: Write the compose file**

Create `cloud/docker-compose.yml`:

```yaml
# Local-first rehearsal stack for the cloud-hosted drive dashboard (design
# doc: docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md, §8
# testing plan item 1). Mirrors the eventual EC2 topology (nginx + Mosquitto)
# so swapping to EC2 later is a configuration change, not a new architecture
# - same spirit as plan.md 2c.1's local/cloud parity argument.
#
# Usage: `docker compose -f cloud/docker-compose.yml up -d`
# Dashboard: http://localhost:8080/  Broker: localhost:1883 (MQTT), :9001 (WS)

services:
  mosquitto:
    image: eclipse-mosquitto:2
    ports:
      - "1883:1883"
      - "9001:9001"
    volumes:
      - ./mosquitto/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro

  dashboard:
    image: nginx:alpine
    ports:
      - "8080:80"
    volumes:
      - ./dashboard:/usr/share/nginx/html:ro
    depends_on:
      - mosquitto
```

- [ ] **Step 3: Validate the broker config and bring the stack up**

Run: `docker run --rm -v "$(pwd)/cloud/mosquitto/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" eclipse-mosquitto:2 mosquitto -c /mosquitto/config/mosquitto.conf -t`
Expected: no error output (the `-t` flag validates config and exits; a syntax error would print a line number and reason).

Run: `cd cloud && docker compose up -d && docker compose ps`
Expected: both `mosquitto` and `dashboard` services show `running`/`Up`.

- [ ] **Step 4: Round-trip test via the CLI (no dashboard/ESP32 needed yet)**

Run, in one terminal: `docker exec -it cloud-mosquitto-1 mosquitto_sub -t 'robot/robot1/telemetry' -v`
Run, in a second terminal: `docker exec -it cloud-mosquitto-1 mosquitto_pub -t 'robot/robot1/telemetry' -m '{"left_a":0.1,"right_a":0.1,"left_duty":10,"right_duty":10,"vbus":12.0}'`
Expected: the `mosquitto_sub` terminal prints the published message immediately. (Container name may differ slightly depending on Docker Compose's project-naming — check with `docker compose ps` if `cloud-mosquitto-1` doesn't match.)

- [ ] **Step 5: Commit**

```bash
git add cloud/mosquitto/mosquitto.conf cloud/docker-compose.yml
git commit -m "Add local Mosquitto rehearsal stack for the cloud-hosted drive dashboard"
```

---

### Task 2: Dashboard MQTT topic/message helpers (TDD)

**Files:**
- Create: `cloud/dashboard/mqtt_topics.js`
- Create: `cloud/dashboard/test/mqtt_topics.test.js`

**Interfaces:**
- Consumes: nothing (pure, dependency-free).
- Produces: `ROBOT_ID` (string constant), `driveTopic(robotId)`, `telemetryTopic(robotId)`, `buildDriveMessage(left, right)` → JSON string, `parseTelemetryMessage(payloadString)` → object or `null`. Task 3's `app.js` and Task 4's `mqtt_handler.h` both depend on these exact topic strings and message shapes (the `.h` file reimplements the C++ equivalent by hand, same split as `mix.js`/`mixDrive` already has no server-side twin).

- [ ] **Step 1: Write the failing test**

Create `cloud/dashboard/test/mqtt_topics.test.js`:

```javascript
// cloud/dashboard/test/mqtt_topics.test.js
const assert = require("assert");
const { driveTopic, telemetryTopic, buildDriveMessage, parseTelemetryMessage } = require("../mqtt_topics.js");

// topic builders match design doc §4 exactly
{
  assert.strictEqual(driveTopic("robot1"), "robot/robot1/drive");
  assert.strictEqual(telemetryTopic("robot1"), "robot/robot1/telemetry");
}

// buildDriveMessage produces the exact shape the ESP32 firmware parses
// (embedded/web_drive/mqtt_handler.h's parseDriveMessage)
{
  const json = buildDriveMessage(-128, 255);
  assert.deepStrictEqual(JSON.parse(json), { left: -128, right: 255 });
}

// parseTelemetryMessage: valid payload round-trips
{
  const payload = JSON.stringify({ left_a: 0.42, right_a: 0.39, left_duty: 180, right_duty: 178, vbus: 12.1 });
  const msg = parseTelemetryMessage(payload);
  assert.strictEqual(msg.vbus, 12.1);
  assert.strictEqual(msg.left_duty, 180);
}

// parseTelemetryMessage: malformed JSON returns null, doesn't throw
{
  assert.strictEqual(parseTelemetryMessage("{not json"), null);
}

// parseTelemetryMessage: valid JSON but missing a required field returns null
{
  const payload = JSON.stringify({ left_a: 0.1, right_a: 0.1, left_duty: 1, right_duty: 1 });   // vbus missing
  assert.strictEqual(parseTelemetryMessage(payload), null);
}

// parseTelemetryMessage: valid JSON but not an object (e.g. a bare number) returns null
{
  assert.strictEqual(parseTelemetryMessage("42"), null);
}

console.log("mqtt_topics.test.js: all assertions passed");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node cloud/dashboard/test/mqtt_topics.test.js`
Expected: `Error: Cannot find module '../mqtt_topics.js'`

- [ ] **Step 3: Write minimal implementation**

Create `cloud/dashboard/mqtt_topics.js`:

```javascript
// mqtt_topics.js - pure helpers for the robot/{id}/* MQTT topics and
// message shapes (design doc docs/superpowers/specs/2026-10-04-cloud-web-
// dashboard-design.md §4). Kept dependency-free and side-effect-free so it
// can be unit tested the same way mix.js is, without a live broker or
// browser. Loadable both as a plain <script> (defines globals) and as a
// Node module (for mqtt_topics.test.js).

var ROBOT_ID = "robot1";   // [ASSUMPTION] single fixed robot id, matches spec.md §6.1's existing assumption

function driveTopic(robotId) {
  return "robot/" + robotId + "/drive";
}

function telemetryTopic(robotId) {
  return "robot/" + robotId + "/telemetry";
}

// left/right: pre-mixed duty, same shape mix.js's mixDrive() already produces.
function buildDriveMessage(left, right) {
  return JSON.stringify({ left: left, right: right });
}

// Returns the parsed telemetry object, or null if the payload isn't valid
// JSON, isn't an object, or is missing a required field. Never throws -
// callers (app.js's MQTT message handler) can treat null as "drop this
// message" without a try/catch of their own.
function parseTelemetryMessage(payload) {
  var msg;
  try { msg = JSON.parse(payload); } catch (e) { return null; }
  if (typeof msg !== "object" || msg === null) return null;
  var required = ["left_a", "right_a", "left_duty", "right_duty", "vbus"];
  for (var i = 0; i < required.length; i++) {
    if (!(required[i] in msg)) return null;
  }
  return msg;
}

if (typeof module !== "undefined") {
  module.exports = {
    ROBOT_ID: ROBOT_ID,
    driveTopic: driveTopic,
    telemetryTopic: telemetryTopic,
    buildDriveMessage: buildDriveMessage,
    parseTelemetryMessage: parseTelemetryMessage,
  };
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node cloud/dashboard/test/mqtt_topics.test.js`
Expected: `mqtt_topics.test.js: all assertions passed`

- [ ] **Step 5: Commit**

```bash
git add cloud/dashboard/mqtt_topics.js cloud/dashboard/test/mqtt_topics.test.js
git commit -m "Add MQTT topic/message helpers for the cloud dashboard"
```

---

### Task 3: Port the dashboard frontend to MQTT-over-WebSocket

**Files:**
- Create: `cloud/dashboard/index.html` (ported from `embedded/web_drive/data/index.html`)
- Create: `cloud/dashboard/style.css` (ported unchanged from `embedded/web_drive/data/style.css`)
- Create: `cloud/dashboard/mix.js` (ported unchanged from `embedded/web_drive/data/mix.js`)
- Create: `cloud/dashboard/app.js` (rewritten from `embedded/web_drive/data/app.js`)
- Create: `cloud/dashboard/test/mix.test.js` (moved from `embedded/web_drive/test/mix.test.js`, require path fixed)
- Delete: `embedded/web_drive/data/` (whole directory, once copied)
- Delete: `embedded/web_drive/test/mix.test.js` (and the now-empty `embedded/web_drive/test/` directory)

**Interfaces:**
- Consumes: `driveTopic`, `telemetryTopic`, `buildDriveMessage`, `parseTelemetryMessage`, `ROBOT_ID` from Task 2's `mqtt_topics.js`; `mixDrive(dx, dy, speedCap)` from `mix.js` (unchanged); the global `mqtt` object from the MQTT.js CDN script.
- Produces: a dashboard served by nginx (Task 1's `dashboard` service) at `http://localhost:8080/`, connecting to the broker's WebSocket listener at `ws://<host>:9001`.

- [ ] **Step 1: Copy the unchanged static assets**

```bash
mkdir -p cloud/dashboard/test
cp embedded/web_drive/data/style.css cloud/dashboard/style.css
cp embedded/web_drive/data/mix.js cloud/dashboard/mix.js
```

- [ ] **Step 2: Move and fix the existing mix.js test**

```bash
git mv embedded/web_drive/test/mix.test.js cloud/dashboard/test/mix.test.js
```

Edit `cloud/dashboard/test/mix.test.js` — change the require path (it moved from `embedded/web_drive/test/` requiring `../data/mix.js` to `cloud/dashboard/test/` requiring `../mix.js`):

```javascript
// Old:
const { mixDrive } = require("../data/mix.js");
// New:
const { mixDrive } = require("../mix.js");
```

- [ ] **Step 3: Run the moved test to confirm the path fix works**

Run: `node cloud/dashboard/test/mix.test.js`
Expected: `mix.test.js: all assertions passed`

- [ ] **Step 4: Write the ported index.html**

Create `cloud/dashboard/index.html` (identical to `embedded/web_drive/data/index.html` except the script tags at the bottom and the title):

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>Robot Drive Dashboard</title>
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
        <div class="battery-gauge-well">
          <div class="battery-fill" id="batteryFill"></div>
        </div>
        <span id="batteryLabel">-- V</span>
      </div>
    </section>
  </div>

  <script src="https://cdn.jsdelivr.net/npm/mqtt@5.10.1/dist/mqtt.min.js"></script>
  <script src="/mix.js"></script>
  <script src="/mqtt_topics.js"></script>
  <script src="/app.js"></script>
</body>
</html>
```

- [ ] **Step 5: Write the rewritten app.js**

Create `cloud/dashboard/app.js`:

```javascript
// app.js - MQTT-over-WebSocket client (via MQTT.js, loaded by index.html
// before this file), joystick input, telemetry rendering. Depends on
// mixDrive() from mix.js and the pure helpers in mqtt_topics.js (both
// loaded before this file in index.html).
//
// Ported from the original web_drive WebSocket client (embedded/web_drive/
// data/app.js) per docs/superpowers/specs/2026-10-04-cloud-web-dashboard-
// design.md - same joystick/telemetry UI, different transport.

(function () {
  // Local-dev broker's WebSocket listener (cloud/mosquitto/mosquitto.conf).
  // On EC2 this becomes a path behind nginx's WSS reverse proxy (design
  // D3) - a configuration-only change, same swap philosophy as plan.md
  // 2c.1, not a code change.
  const MQTT_WS_URL = "ws://" + location.hostname + ":9001";
  const KEEPALIVE_MS = 150;   // well under the ESP32's 1500ms watchdog (design D6)

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

  let client = null;
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
    // Gated on `connected`, not just `client` existing: MQTT.js queues
    // outgoing QoS-0 publishes made while disconnected and flushes them on
    // reconnect by default - without this gate, drive commands sent while
    // the joystick is held through a disconnect would all fire at once the
    // moment the link comes back, which is exactly the "stale command
    // executes late" risk the design doc's §5 flags. Not calling publish()
    // at all while disconnected sidesteps that queue entirely.
    if (!connected || !client) return;
    const cap = speedCap.value / 100;
    const { left, right } = mixDrive(dx, dy, cap);
    client.publish(driveTopic(ROBOT_ID), buildDriveMessage(left, right));
  }

  function connect() {
    const c = mqtt.connect(MQTT_WS_URL, { reconnectPeriod: 1000 });
    client = c;

    c.on("connect", () => {
      connected = true;
      setStatus("connected");
      setWarning("");
      c.subscribe(telemetryTopic(ROBOT_ID));
      if (dragging) startKeepalive();
    });

    c.on("reconnect", () => {
      connected = false;
      setStatus("disconnected");
      setWarning("LINK LOST");
      stopKeepalive();
    });

    c.on("close", () => {
      connected = false;
      setStatus("disconnected");
      setWarning("LINK LOST");
      stopKeepalive();
    });

    c.on("error", () => {
      // MQTT.js's own reconnectPeriod handles retry - nothing to do here
      // beyond what "close"/"reconnect" already cover.
    });

    c.on("message", (topic, payloadBuf) => {
      if (topic !== telemetryTopic(ROBOT_ID)) return;
      const msg = parseTelemetryMessage(payloadBuf.toString());
      if (msg) renderTelemetry(msg);
    });
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
  // Avoid the arcs rendering as full circles (misread as "max current")
  // before the first telemetry message arrives.
  setArc(currentLeftArc, 80, 0);
  setArc(currentRightArc, 60, 0);
  connect();
})();
```

- [ ] **Step 6: Remove the old local-AP copies**

```bash
rm -rf embedded/web_drive/data
rmdir embedded/web_drive/test 2>/dev/null || true
```

(`embedded/web_drive/test/` should now be empty since Step 2 already moved `mix.test.js` out — `rmdir` will no-op harmlessly if something unexpected is still in there, which is worth investigating rather than force-deleting.)

- [ ] **Step 7: Bring up the local stack and manually verify in a browser**

Run: `cd cloud && docker compose up -d` (if not already running from Task 1)
Open `http://localhost:8080/` in a browser.
Expected: status ring shows "disconnected" briefly, then flips to "connected" within ~1s (MQTT.js's default connect handshake).

Run (separate terminal): `docker exec -it cloud-mosquitto-1 mosquitto_pub -t 'robot/robot1/telemetry' -m '{"left_a":0.5,"right_a":0.3,"left_duty":120,"right_duty":90,"vbus":11.8}'`
Expected: the dashboard's VBUS readout updates to "11.8 V" and the current arcs move, within ~1s, with no page reload.

Drag the joystick; run `docker exec -it cloud-mosquitto-1 mosquitto_sub -t 'robot/robot1/drive' -v` in a third terminal before dragging.
Expected: `mosquitto_sub` prints a stream of `{"left":N,"right":N}` messages at roughly the 150ms keepalive rate while dragging, and stops within ~150ms of release.

Stop the broker (`docker compose stop mosquitto`) while the joystick is held down.
Expected: status ring flips to "disconnected" / "LINK LOST" within ~1-2s, and the `mosquitto_sub` terminal (once the broker is restarted) shows no backlog of queued messages suddenly flushing.

- [ ] **Step 8: Commit**

```bash
git add cloud/dashboard embedded/web_drive/data embedded/web_drive/test
git commit -m "Port web_drive dashboard frontend to MQTT-over-WebSocket"
```

(The `git add` of now-deleted paths stages the deletions; `git status` should show the `data/`/`test/` files as deleted and the new `cloud/dashboard/` files as added.)

---

### Task 4: ESP32 firmware: Wi-Fi station + MQTT client port

**Files:**
- Modify: `embedded/web_drive/web_drive.ino`
- Create: `embedded/web_drive/mqtt_handler.h`
- Delete: `embedded/web_drive/ws_handler.h`
- (Unchanged: `embedded/web_drive/drive_core.h`)

**Interfaces:**
- Consumes: `driveSetup()`, `drive(left, right)`, `stopAll()`, `readCurrents()`, `readBusVoltage()`, `isense[LEFT/RIGHT].amps`, `lastDuty[LEFT/RIGHT]` from the unchanged `drive_core.h`.
- Produces: `mqttSetup()`, `mqttLoop()`, `mqttSendTelemetry()` — called from `web_drive.ino`'s `setup()`/`loop()`.
- Requires the **PubSubClient** Arduino library (by Nick O'Leary — Arduino Library Manager / PlatformIO registry) installed before compiling. `[VERIFY]` its default `MQTT_MAX_PACKET_SIZE` (commonly 256 bytes in current releases) comfortably covers the ~110-byte telemetry JSON below; if a future telemetry shape grows past that, `MQTT_MAX_PACKET_SIZE` needs raising via a build flag, not silently truncating.

- [ ] **Step 1: Delete the old WebSocket handler**

```bash
git rm embedded/web_drive/ws_handler.h
```

- [ ] **Step 2: Write the new MQTT handler**

Create `embedded/web_drive/mqtt_handler.h`:

```cpp
#pragma once

// mqtt_handler.h - MQTT client (PubSubClient over a plain WiFiClient for
// local-dev; swap to WiFiClientSecure for the real EC2/TLS endpoint, design
// D2/D4 - not yet wired up here, local-dev only, see UPDATES.md), the
// robot/{id}/drive subscription and robot/{id}/telemetry publish (design
// doc docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md §4),
// and the command watchdog. Includes drive_core.h itself (safe under its
// #pragma once guard even though web_drive.ino also includes it directly).

#include <WiFi.h>
#include <PubSubClient.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "drive_core.h"

// [TBD] point this at your dev machine's LAN IP running
// cloud/docker-compose.yml's mosquitto service (design doc §8 item 1).
// The ESP32 can't resolve "localhost" to the dev machine - it needs a real
// reachable address on the same Wi-Fi network as WIFI_SSID in web_drive.ino.
static const char *MQTT_BROKER_HOST = "192.0.2.1";
static const uint16_t MQTT_BROKER_PORT = 1883;
static const char *ROBOT_ID = "robot1";   // [ASSUMPTION] must match cloud/dashboard/mqtt_topics.js's ROBOT_ID

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static volatile uint32_t lastDriveMsgMs = 0;
static const uint32_t WATCHDOG_MS = 1500;   // design D6 - raised from the local-AP sketch's 400ms for cloud-relay latency/jitter

static char driveTopicBuf[32];
static char telemetryTopicBuf[32];

// ---- inbound: {"left":N,"right":N} on robot/{id}/drive ----
// Hand-rolled parsing, not a JSON library - unchanged logic from the
// local-AP version's ws_handler.h, just reading an MQTT payload instead of
// a WS frame. A malformed or out-of-range message returns false and is
// dropped silently - the watchdog is NOT reset, so a garbled command
// degrades to "eventually stops," never "drives on bad data."
static bool parseDriveMessage(const char *json, int *left, int *right) {
  const char *lp = strstr(json, "\"left\"");
  const char *rp = strstr(json, "\"right\"");
  if (!lp || !rp) return false;

  lp = strchr(lp, ':');
  rp = strchr(rp, ':');
  if (!lp || !rp) return false;

  char *lend = nullptr;
  char *rend = nullptr;
  long l = strtol(lp + 1, &lend, 10);
  long r = strtol(rp + 1, &rend, 10);
  if (lend == lp + 1 || rend == rp + 1) return false;
  if (l < -255 || l > 255 || r < -255 || r > 255) return false;

  *left  = (int)l;
  *right = (int)r;
  return true;
}

static void mqttMessageReceived(char *topic, byte *payload, unsigned int length) {
  // 192 bytes comfortably covers {"left":-255,"right":-255} with room to
  // spare; anything bigger is rejected rather than truncated-and-parsed,
  // since a truncated payload could silently parse into a wrong-but-valid-
  // looking command.
  static const size_t MAX_PAYLOAD = 192;
  if (length >= MAX_PAYLOAD) return;

  char buf[MAX_PAYLOAD];
  memcpy(buf, payload, length);
  buf[length] = '\0';

  int left, right;
  if (parseDriveMessage(buf, &left, &right)) {
    // Record the timestamp BEFORE applying the command, same ordering
    // reasoning as the local-AP version: a watchdog check landing between
    // drive() and the timestamp write would otherwise see a stale
    // timestamp and immediately stopAll() right after this command was
    // applied.
    lastDriveMsgMs = millis();
    drive(left, right);
  }
}

// Command watchdog: must run every loop() iteration regardless of MQTT
// connection state - this is the ONLY thing that stops the motors on a
// dropped link, same role as the local-AP version's wsWatchdogCheck().
static void mqttWatchdogCheck() {
  if (millis() - lastDriveMsgMs > WATCHDOG_MS) {
    stopAll();
  }
}

static void mqttSetup() {
  snprintf(driveTopicBuf, sizeof(driveTopicBuf), "robot/%s/drive", ROBOT_ID);
  snprintf(telemetryTopicBuf, sizeof(telemetryTopicBuf), "robot/%s/telemetry", ROBOT_ID);

  mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  mqttClient.setCallback(mqttMessageReceived);
}

static void mqttReconnect() {
  // client.connect() blocks for up to a few seconds against an unreachable
  // broker - long enough to blow past WATCHDOG_MS while loop() is stuck
  // here. stopAll() FIRST so the robot never coasts at a stale duty for
  // the duration of a blocking reconnect attempt; the watchdog's own
  // periodic check can't save us here because it doesn't run again until
  // this call returns.
  stopAll();

  String clientId = "esp32-" + String(ROBOT_ID);
  if (mqttClient.connect(clientId.c_str())) {
    mqttClient.subscribe(driveTopicBuf);
    Serial.println("MQTT connected and subscribed");
  }
}

// Services the MQTT client and the command watchdog together, every
// loop() iteration. Reconnect attempts are rate-limited (not every single
// loop iteration) so a persistently unreachable broker doesn't spend all
// of loop()'s time blocked in connect() - between attempts, the watchdog
// keeps running freely and motors stay stopped (no valid drive message can
// arrive while disconnected, so WATCHDOG_MS naturally covers this gap too).
static void mqttLoop() {
  mqttWatchdogCheck();

  if (!mqttClient.connected()) {
    static uint32_t nextReconnectAttemptMs = 0;
    if ((int32_t)(millis() - nextReconnectAttemptMs) >= 0) {
      nextReconnectAttemptMs = millis() + 5000;
      mqttReconnect();
    }
    return;
  }

  mqttClient.loop();
}

// Push telemetry, same JSON shape as the local-AP version's
// wsSendTelemetry(), now via MQTT publish instead of a WS frame.
static void mqttSendTelemetry() {
  if (!mqttClient.connected()) return;

  readCurrents();

  float leftAmps  = isnan(isense[LEFT].amps)  ? 0.0f : isense[LEFT].amps;
  float rightAmps = isnan(isense[RIGHT].amps) ? 0.0f : isense[RIGHT].amps;

  char json[192];
  int n = snprintf(json, sizeof(json),
    "{\"left_a\":%.3f,\"right_a\":%.3f,"
    "\"left_duty\":%d,\"right_duty\":%d,\"vbus\":%.2f}",
    leftAmps, rightAmps,
    lastDuty[LEFT], lastDuty[RIGHT], readBusVoltage());

  mqttClient.publish(telemetryTopicBuf, (const uint8_t *)json, n, false);
}
```

- [ ] **Step 3: Rewrite web_drive.ino**

Replace the full contents of `embedded/web_drive/web_drive.ino`:

```cpp
/*
 * web_drive.ino - landscape browser drive control + telemetry, now relayed
 * through the cloud (EC2 Mosquitto broker) instead of the ESP32's own
 * Wi-Fi AP. See docs/superpowers/specs/2026-10-04-cloud-web-dashboard-
 * design.md for the full design; this sketch is still the same "bring-up
 * tool" scope as before (no SAFE_HOLD, no mode state machine, spec.md §5)
 * - only the transport changed, from local AP+WebSocket to Wi-Fi-station+
 * MQTT.
 *
 * Two things still stand in for the real safety stack:
 *   - a voltage-based duty clamp (drive_core.h, from drive_calib.ino) so
 *     3.0-6.0V motors never see more than ~5.5V from this 12V bus
 *   - a command watchdog (mqtt_handler.h): no valid "drive" message ->
 *     motors stop. Timeout raised from 400ms (local-AP) to 1500ms (design
 *     D6) to tolerate ordinary internet/cloud-relay latency/jitter without
 *     false-tripping.
 *
 * WHEELS OFF THE GROUND for first bring-up - not yet bench-tested with this
 * MQTT transport (see UPDATES.md).
 *
 * The dashboard frontend (index.html/style.css/mix.js/mqtt_topics.js/
 * app.js) no longer lives here - it moved to cloud/dashboard/, served by
 * nginx (locally, or on EC2), not LittleFS. No LittleFS image needed for
 * this sketch anymore.
 */

#include <WiFi.h>

#include "drive_core.h"
#include "mqtt_handler.h"

// [TBD] real on-site Wi-Fi credentials - these are LOCAL-DEV placeholders
// only (point at a Mosquitto-in-Docker host on your dev LAN, design doc §8
// item 1). Do not field this sketch with these values.
static const char *WIFI_SSID     = "your-dev-wifi";
static const char *WIFI_PASSWORD = "your-dev-wifi-password";

static uint32_t telemetryNextMs = 0;
static const uint32_t TELEMETRY_MS = 200;   // ~5 Hz, unchanged from the local-AP version

void setup() {
  Serial.begin(115200);
  delay(300);

  driveSetup();   // kills both bridges first, then brings up pins + INA219s

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    // Blocking by design, only at boot: nothing meaningful to drive yet
    // (driveSetup() above already forced motors to zero), so there's no
    // watchdog to stall here the way there would be once loop() starts.
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Wi-Fi connected, IP: ");
  Serial.println(WiFi.localIP());

  mqttSetup();
  Serial.println("MQTT client configured. Connecting in loop()...");
}

void loop() {
  mqttLoop();   // services the MQTT client, (re)connects as needed, and
                // runs the command watchdog on every iteration - see
                // mqtt_handler.h's mqttLoop() for why reconnect and the
                // watchdog can't be two independent blocking steps here.

  if ((int32_t)(millis() - telemetryNextMs) >= 0) {
    telemetryNextMs = millis() + TELEMETRY_MS;
    mqttSendTelemetry();
  }

  delay(2);
}
```

- [ ] **Step 4: Bench-check against the local broker (manual — no automated test exists for ESP32 C++ in this repo)**

Prerequisite: install the PubSubClient library; set `WIFI_SSID`/`WIFI_PASSWORD` in `web_drive.ino` to your dev Wi-Fi, and `MQTT_BROKER_HOST` in `mqtt_handler.h` to your dev machine's LAN IP (the one running Task 1's `docker compose up -d`). **Wheels off the ground.**

1. Flash, open Serial Monitor at 115200. Expected: `Wi-Fi connected, IP: ...` then `MQTT connected and subscribed`.
2. From the dashboard (Task 3) or `mosquitto_pub -t robot/robot1/drive -m '{"left":100,"right":100}'`: confirm both wheels spin the same direction at a matching speed.
3. Stop sending drive messages (release the joystick, or stop the `mosquitto_pub` loop): confirm motors stop within ~1.5s (watchdog).
4. Stop the Docker broker (`docker compose stop mosquitto` on the dev machine) while a wheel is spinning: confirm the motor stops within ~1.5s, **not** several extra seconds (this is the `stopAll()`-before-blocking-`connect()` behavior in `mqttReconnect()` — if it takes longer than ~1.5-2s, that guard isn't working as intended and needs revisiting before calling this task done).
5. Restart the broker: confirm the ESP32 reconnects and resumes taking drive commands within the 5s reconnect-attempt interval, without a manual reset.
6. Confirm telemetry (`mosquitto_sub -t robot/robot1/telemetry -v`) shows plausible current/voltage values while driving.

- [ ] **Step 5: Commit**

```bash
git add embedded/web_drive/web_drive.ino embedded/web_drive/mqtt_handler.h embedded/web_drive/ws_handler.h
git commit -m "Port ESP32 web_drive firmware from local-AP+WebSocket to Wi-Fi-station+MQTT"
```

---

### Task 5: spec.md full sweep — ESP32↔EC2-direct architecture

**Files:**
- Modify: `spec.md` (FR-2, FR-7, FR-9, NFR-2, NFR-3, NFR-4, §3 diagram + boundary bullets, §5, §6.1/§6.2 merge, §6.3 pointer update, §7 assumptions 1/4/5/6, §8 acceptance rows, §9)

**Interfaces:**
- No code interface — this is a documentation-consistency task. "Done" means: no remaining `spec.md` text implies a phone-bridge device exists in the current architecture (historical/superseded-pointer text, like the existing §6.3 convention, is fine and expected).

- [ ] **Step 1: FR-2 — Manual mode**

```
Old:
**FR-2 — Manual mode.** Operator drives the robot directly via phone GUI controls (e.g. virtual joystick/d-pad). Commands pass phone → ESP32 with no cloud dependency.

New:
**FR-2 — Manual mode.** Operator drives the robot directly via browser dashboard controls (virtual joystick, `cloud/dashboard/`). Commands pass dashboard → EC2 broker → ESP32 (§6.1) — unlike the earlier phone-bridge design, this path now **does** cross the cloud broker; §9 explains why that's still an acceptable trade (the watchdog-based fail-safe doesn't depend on the broker's cooperation).
```

- [ ] **Step 2: FR-7 — Mapping**

```
Old:
**FR-7 — Mapping.** EC2 builds and maintains an occupancy/pose-graph map via SLAM Toolbox from range-scan + odometry data relayed by the phone. The scan itself is assembled on the ESP32 from a single-point range sensor mechanically swept by a servo (see §4/§7), not a spinning 2D lidar. The map is available to the GUI for operator situational awareness and to Nav2/D*-Lite for planning.

New:
**FR-7 — Mapping.** EC2 builds and maintains an occupancy/pose-graph map via SLAM Toolbox from range-scan + odometry data published directly by the ESP32 (no phone relay — see §6.1; not yet implemented, see that section's status table). The scan itself is assembled from a single-point range sensor mechanically swept by a servo (see §4/§7), not a spinning 2D lidar. The map is available to the dashboard for operator situational awareness and to Nav2/D*-Lite for planning.
```

- [ ] **Step 3: FR-9 — Comms-loss fail-safe**

```
Old:
**FR-9 — Comms-loss fail-safe.** Loss of ESP32↔phone or phone↔EC2 communication does not result in unsafe motion. See §5, §8.

New:
**FR-9 — Comms-loss fail-safe.** Loss of the ESP32↔EC2 MQTT link does not result in unsafe motion (no phone-relay hop exists anymore — see §6.1). See §5, §8.
```

- [ ] **Step 4: NFR-2, NFR-3, NFR-4**

```
Old (NFR-2 Notes column):
Fault → SAFE_HOLD entry in `[ASSUMPTION] <100 ms`, computed entirely on ESP32, independent of phone or cloud availability.

New:
Fault → SAFE_HOLD entry in `[ASSUMPTION] <100 ms`, computed entirely on ESP32, independent of cloud availability.
```

```
Old (full NFR-3 row):
| NFR-3 | ESP32↔phone latency | `[ASSUMPTION] <50 ms` typical, over the ESP32-hosted local Wi-Fi link (see §7). `[VERIFY]` more strongly than a wired link would need — RF conditions (distance, interference, contention) vary in a way a USB tether doesn't; measure on real hardware in the intended operating environment before trusting this budget. |

New:
| NFR-3 | `[REMOVED]` ESP32↔phone latency | Superseded — no ESP32↔phone link exists anymore (`docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`). The ESP32's only wireless hop now is ESP32↔EC2, covered by NFR-4. Row number kept as a pointer for readers of earlier revisions, same convention as §6.3. |
```

```
Old (full NFR-4 row):
| NFR-4 | Phone↔EC2 latency | Not bounded tightly — variable over cellular/Wi-Fi, budgeted 100 ms–2000+ ms `[VERIFY: measure once real network path exists]`. Explicitly outside the safety path (NFR-2 must not depend on this). |

New:
| NFR-4 | ESP32↔EC2 latency | Not bounded tightly — variable over Wi-Fi (the ESP32 itself has no cellular radio in the current sensor list, §4/§7 — outdoor range is now bounded by reachable Wi-Fi, e.g. a mobile hotspot near the robot, until that's revisited), budgeted 100 ms–2000+ ms `[VERIFY: measure once real network path exists]`. Explicitly outside the safety path (NFR-2 must not depend on this) — **except** manual-drive commands (FR-2) now ride this same link, which they didn't in the old phone-bridge design; see §9. |
```

- [ ] **Step 5: §3 System Boundary Diagram + boundary ownership bullets**

```
Old diagram:
                       (wireless — ESP32 hosts its own Wi-Fi AP, see §7 ASSUMPTION)
   ┌─────────────────┐   Wi-Fi, UDP framed JSON   ┌──────────────────────┐
   │      ESP32       │◄──────────────────────────►│   Android Phone      │
   │   (on-robot)      │                            │  (handheld bridge   │
   │                    │                            │   + GUI)             │
   │ - motor PID         │                            │                      │
   │ - IMU (I2C)          │                            │  - GUI (Manual /     │
   │ - GPS (UART)          │                            │    Autonomous /      │
   │ - curr/volt sensing    │                            │    Follow-me)        │
   │ - range sensor +         │                            │  - relays telemetry  │
   │   sweep servo (scan)      │                            │  - relays scan +     │
   │ - electromag driver         │                          │    commands          │
   │ - SAFE_HOLD interlock         │                        └──────────┬───────────┘
   │ - Wi-Fi AP host                 │                                  │
   └─────────────────┘                                        MQTT/TLS │
                                                     (cellular or Wi-Fi) │
                                                                          ▼
                                                             ┌──────────────────────────┐
                                                             │   EC2 (Linux instance)   │
                                                             │                          │
                                                             │  - SLAM Toolbox          │
                                                             │  - Nav2                  │
                                                             │  - D*-Lite global        │
                                                             │    planner loop          │
                                                             │  - map store             │
                                                             └──────────────────────────┘

New diagram:
   ┌─────────────────────┐
   │        ESP32         │
   │      (on-robot)       │
   │                        │
   │ - motor PID             │
   │ - IMU (I2C)              │
   │ - GPS (UART)              │
   │ - curr/volt sensing        │
   │ - range sensor +             │
   │   sweep servo (scan)          │
   │ - electromag driver             │
   │ - SAFE_HOLD interlock             │
   │ - Wi-Fi STATION (joins local Wi-Fi) │
   └─────────────────┬────────────────┘
                      │  MQTT/TLS, outbound-only from the ESP32
                      │  (Wi-Fi; see §7 assumption 6 on the cellular-
                      │   mobility consequence of dropping the phone bridge)
                      ▼
          ┌──────────────────────────────────┐
          │       EC2 (Linux instance)        │
          │                                    │
          │  - Mosquitto broker (TLS + auth)    │
          │  - nginx (serves browser dashboard)  │
          │  - SLAM Toolbox                       │
          │  - Nav2                                │
          │  - D*-Lite global planner loop          │
          │  - map store                              │
          └───────────────────┬──────────────────────┘
                               │  HTTPS/WSS
                               ▼
               Any phone/tablet/laptop (browser dashboard,
               anywhere with internet — no longer a bridge
               device, just a viewer/controller of EC2)
```

```
Old boundary bullets:
- **ESP32**: real-time control, real-time safety, local reflex behavior, *and* now the range-sensing hardware (single-point sensor + sweep servo, see §4/§7) since the phone is no longer robot-mounted. Nothing on this side of the boundary may block on phone or cloud availability.
- **Phone**: handheld bridge + GUI. No control authority of its own beyond forwarding operator intent and a local e-stop button press; not a safety-critical compute node (see Warnings in plan.md 2c for the implication of a phone-software crash). No longer hosts any sensor directly — it relays scan data the ESP32 assembles, same as it relays telemetry.
- **EC2**: heavy math only — SLAM, global planning. Never in the real-time safety path.

New boundary bullets:
- **ESP32**: real-time control, real-time safety, local reflex behavior, and the range-sensing hardware (single-point sensor + sweep servo, see §4/§7). Nothing on this side of the boundary may block on cloud availability (no phone hop exists to block on either).
- **Browser dashboard** (replaces the Android-app-on-a-phone-bridge role entirely — `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`): runs in any browser, served from EC2. No control authority of its own beyond forwarding operator intent; not a safety-critical compute node, and not in the data path between ESP32 and EC2 at all — it's a client of the same MQTT broker the ESP32 talks to, not a relay sitting between them.
- **EC2**: heavy math (SLAM, global planning) plus now hosting the MQTT broker and serving the browser dashboard's static files (design doc §3). Never in the real-time safety path regardless.
```

- [ ] **Step 6: §5 Mode State Machine — "phone GUI" references**

```
Old:
**Authority**: the ESP32 is the single source of truth for the *current confirmed mode*. The phone GUI only *requests* a mode; the ESP32 accepts or rejects the request and reports the confirmed mode back. This follows directly from "ESP32 owns real-time safety" — mode arbitration is a safety-relevant decision (e.g. don't enter Autonomous without a valid plan) and must not depend on the phone or cloud being correct.

New:
**Authority**: the ESP32 is the single source of truth for the *current confirmed mode*. The browser dashboard only *requests* a mode; the ESP32 accepts or rejects the request and reports the confirmed mode back. This follows directly from "ESP32 owns real-time safety" — mode arbitration is a safety-relevant decision (e.g. don't enter Autonomous without a valid plan) and must not depend on the dashboard or cloud being correct.
```

```
Old (transition table row):
| any mode | SAFE_HOLD | Heartbeat loss (ESP32↔phone), overcurrent/stall fault, IMU fault, e-stop command | Motors bounded-stop (or immediate cutoff on overcurrent trip); queued waypoints/target-lock discarded; electromagnet state **latched**, not auto-dropped, unless brownout-protection logic requires load shedding (see plan.md Warnings) |

New:
| any mode | SAFE_HOLD | Heartbeat loss (ESP32↔EC2), overcurrent/stall fault, IMU fault, e-stop command | Motors bounded-stop (or immediate cutoff on overcurrent trip); queued waypoints/target-lock discarded; electromagnet state **latched**, not auto-dropped, unless brownout-protection logic requires load shedding (see plan.md Warnings) |
```

```
Old:
Rejected mode-change requests return a reason code to the phone (e.g. `NO_VALID_PLAN`, `NO_TARGET_LOCK`, `ACTIVE_FAULT`) so the GUI can explain the refusal rather than silently ignoring it.

New:
Rejected mode-change requests return a reason code to the dashboard (e.g. `NO_VALID_PLAN`, `NO_TARGET_LOCK`, `ACTIVE_FAULT`) so it can explain the refusal rather than silently ignoring it.
```

- [ ] **Step 7: Merge §6.1/§6.2 into one ESP32↔EC2 contract**

Replace the entire `### 6.1 ESP32 ↔ Phone` through `### 6.2 Phone ↔ EC2` block (everything from the `### 6.1` heading up to, but not including, `### 6.3`) with:

```markdown
### 6.1 ESP32 ↔ EC2

Single hop now — this section replaces the former two-hop ESP32↔Phone (UDP)
+ Phone↔EC2 (MQTT) design (`docs/superpowers/specs/2026-10-04-cloud-web-
dashboard-design.md`). The Android-app-as-bridge role is cancelled; any
browser (phone, tablet, laptop) can run the dashboard, but it is a client of
the same broker the ESP32 talks to, not an intermediary in this contract.

- **Transport**: `[ASSUMPTION]` MQTT over TLS, ESP32 as an outbound-only
  client (never accepts inbound connections) — chosen (design D1/D2) over
  keeping the ESP32's own Wi-Fi AP and tunneling it, specifically to reuse
  the broker's TLS+auth instead of exposing a second, separately-secured
  surface. Same broker software locally (Mosquitto in Docker) and on EC2
  (Mosquitto on the instance) — only the hostname/port/credentials change,
  not the protocol or topic/payload shapes (same "drop-in replacement" hard
  constraint as before).
- **Topics** (namespaced by robot id, `[ASSUMPTION]` single robot, id fixed
  for now):

  | Topic | Direction | Content | Rate | Status |
  |---|---|---|---|---|
  | `robot/{id}/drive` | dashboard→ESP32 | pre-mixed duty, `{"left": -255, "right": 255}` | event-driven, keepalive while held | **Implemented** (`embedded/web_drive/`, `cloud/dashboard/`) |
  | `robot/{id}/telemetry` | ESP32→dashboard/cloud | `{"left_a":0.42,"right_a":0.39,"left_duty":180,"right_duty":178,"vbus":12.1}` | `[ASSUMPTION] 5 Hz` | **Implemented** — reduced shape (drive bring-up only); the full target shape below is still `[TBD]`, not built |
  | `robot/{id}/scan` | ESP32→cloud | range-scan, `LaserScan`-shaped | rate gated by sensor+servo sweep speed `[TBD/VERIFY]` | **Not implemented.** `[TBD]` — previously assembled by the phone from per-sample messages; with no phone, whether the ESP32 assembles a full scan itself before publishing, or publishes raw samples for EC2 to assemble, is an open design question, not decided by the 2026-10-04 design |
  | `robot/{id}/odom` | ESP32→cloud | odometry estimate (IMU/wheel-derived) | `[ASSUMPTION] matches scan rate or faster` | Not implemented |
  | `robot/{id}/map` | cloud→dashboard | current map, GUI display | on-change, low rate | Not implemented |
  | `robot/{id}/waypoints` | cloud→ESP32 | D*-Lite output path | event-driven, on (re)plan | Not implemented |
  | `robot/{id}/cmd` | dashboard→cloud | e.g. "compute path to goal (x,y)" | event-driven | Not implemented |
  | `robot/{id}/mode` | ESP32→cloud, dashboard→ESP32 (request) | mode changes/requests | event-driven | Not implemented — mode state machine (§5) doesn't exist on the ESP32 yet |

  **Target telemetry shape** (not yet implemented — current implementation
  is the reduced `robot/{id}/telemetry` row above):
  ```json
  {"seq": 1234, "ts_ms": 88213001, "mode": "AUTONOMOUS",
   "imu": {"roll": 0.4, "pitch": 6.1, "yaw": 122.3},
   "gps": {"lat": 0.0, "lon": 0.0, "fix": true, "hdop": 1.2},
   "motors": {"fl": {"duty": 0.62, "current_a": 1.1}, "fr": {...}, "rl": {...}, "rr": {...}},
   "incline_comp": {"active": true, "est_grade_deg": 8.5, "power_boost_pct": 18},
   "electromagnet": {"state": "off"},
   "battery": {"voltage": 11.8, "soc_pct": 64},
   "fault_flags": []}
  ```
  **Note on `motors.*.current_a` granularity** (unchanged from before):
  sensing is per-drive-channel (2x current sensor, left/right), not
  per-wheel — `fl`/`rl` (left side) always report the identical value, and
  likewise `fr`/`rr` (right side).
- **Commands** (target shape, not yet implemented beyond `drive`):
  ```json
  {"seq": 5567, "ts_ms": 88213050, "cmd_type": "waypoint_list",
   "payload": {"waypoints": [{"x":1.2,"y":0.4},{"x":2.0,"y":1.1}], "plan_id": "p-991"}}
  ```
  `cmd_type` ∈ `{mode_request, manual_drive, waypoint_list, estop, magnet_trigger, heartbeat}`.
  Today's implemented `robot/{id}/drive` message is a simplified stand-in
  for `manual_drive` — `[TBD]` whether it's folded into this `cmd_type`
  envelope later or stays its own topic.
- **Fail-safe**: no valid `drive` message within the ESP32's watchdog window
  (`[ASSUMPTION] 1500 ms`, design D6 — raised from the old ESP32↔phone
  link's 500ms to tolerate cloud-relay latency/jitter) → ESP32 forces
  `drive(0,0)` unilaterally. This doesn't need the broker's cooperation: a
  dead broker connection looks identical to "no command arrived." The full
  mode-machine version of this (SAFE_HOLD, §5) isn't implemented yet —
  today's watchdog only zeroes drive output, it doesn't have a mode to
  transition.
- **Latency budget**: NFR-4 (renamed from "Phone↔EC2" — see NFR table).
  Manual-mode drive commands now cross this budget too (they didn't in the
  old phone-bridge design, where phone→ESP32 was a separate, tighter-budget
  local link) — see §9 for why this is still an acceptable trade, and the
  design doc's §5 for the full safety argument.
- **Who owns real-time safety**: ESP32, unconditionally — same as before.

### 6.2 [Removed] Phone ↔ EC2

Superseded — merged into §6.1 above now that the ESP32 talks to EC2
directly. Heading number kept as a pointer for readers of earlier
revisions, same convention as §6.3 below.
```

- [ ] **Step 8: §6.3 pointer update**

```
Old:
Superseded — the range sensor now lives on the ESP32 (single-point sensor + sweep servo, §4/§7), not on the phone via USB-OTG. Scan data instead flows ESP32 → Phone as `scan_sample` messages over the wireless link in §6.1, and Phone → EC2 unmodified in shape via §6.2's `robot/{id}/scan` topic. This heading number is kept as a pointer for readers of earlier revisions rather than reused for something unrelated.

New:
Superseded — the range sensor now lives on the ESP32 (single-point sensor + sweep servo, §4/§7), not on the phone via USB-OTG. Scan data flows directly ESP32 → EC2 via §6.1's `robot/{id}/scan` topic (not yet implemented, see §6.1's status table). This heading number is kept as a pointer for readers of earlier revisions rather than reused for something unrelated.
```

- [ ] **Step 9: §7 assumptions 1, 4, 5, 6**

```
Old (assumption 1):
1. **Phone is a handheld device carried by the operator**, not mounted on the robot. This is why Follow-me needs to work via onboard sensing rather than "follow the phone's GPS" — that part of the original design is unchanged. It's also why the range-sensing hardware had to move onto the robot itself rather than staying on the phone (see assumption 2 and §4) — a USB-OTG-hosted sensor doesn't work if the host isn't on the robot.

New:
1. **No phone-bridge device exists in this architecture.** The browser dashboard (any phone/tablet/laptop with a browser) talks to the ESP32 only indirectly, via the EC2 MQTT broker (§6.1), never a direct local link to the robot — superseding the earlier "phone is a handheld bridge" assumption entirely (`docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`). The range-sensing hardware still lives on the ESP32, not on any operator device — that placement decision doesn't depend on whether a phone bridge exists, only on the sensor needing to be robot-mounted to be useful for Follow-me/SLAM.
```

```
Old (assumption 4):
4. **ESP32 hosts its own Wi-Fi access point; the phone connects to it directly** as a client (§6.1), replacing the earlier wired-USB-serial design now that the phone is handheld (assumption 1). Chosen over BLE for the throughput/latency headroom scan relay needs, and over depending on external Wi-Fi infrastructure. Requires WPA2/WPA3 auth — see the security note in §6.1.

New:
4. **ESP32 is a Wi-Fi *station* (not an access point)**, joining whatever Wi-Fi network has internet access on-site, and connects outbound-only to the EC2 MQTT broker over TLS (§6.1) — replacing the earlier "ESP32 hosts its own AP, phone connects to it" design now that there's no phone to connect. Chosen (design D1/D2) over keeping the AP and tunneling it, specifically to reuse the broker's TLS+auth instead of exposing a second, weaker-auth surface.
```

```
Old (assumption 5):
5. **Phone↔EC2 uses self-hosted MQTT (Mosquitto-class broker)**, not a managed service like AWS IoT Core, specifically so the local-sim and cloud endpoints are interface-identical (hard constraint). This is a deliberate simplicity/parity choice over IoT Core's extra managed features (device shadows, rules engine) which the project doesn't currently need.

New:
5. **ESP32↔EC2 uses self-hosted MQTT (Mosquitto-class broker)**, TLS + username/password auth (design D4/D5), not a managed service like AWS IoT Core — same broker software and topic/payload-contract parity argument as before, just one hop instead of two (no phone relay in between).
```

```
Old (assumption 6):
6. **Operator range is bounded by the ESP32 AP's Wi-Fi range** (open-air, tens of meters typical, less through obstacles — `[VERIFY]` once an ESP32 variant/antenna is chosen). Walking out of range is not a distinct failure mode requiring new logic: it presents as heartbeat loss and is handled by the existing `SAFE_HOLD` path (§6.1, §5). This is a new hard operational constraint versus the old wired design (which had no meaningful range limit) and should be communicated to the operator via the GUI, not just silently discovered as a `SAFE_HOLD` event.

New:
6. **Operator/robot range is now bounded by Wi-Fi availability, not a dedicated AP's range.** Since the ESP32 is a Wi-Fi station rather than an AP, the robot must be within range of *some* Wi-Fi network with internet access (home Wi-Fi, a mobile hotspot carried near the robot, etc.). **New consequence not present in the phone-bridge design**: the ESP32 itself has no cellular radio in the current sensor list (§4, `ESP32 variant` still `[TBD]`) — outdoor/away-from-fixed-Wi-Fi operation now requires an external Wi-Fi hotspot reachable by the robot, or a cellular-capable ESP32 variant/add-on not currently planned. The old design's cellular-mobility assumption (`plan.md` 2c.2, written when the *phone* carried the cellular radio) needs re-review in light of this — flagged in `UPDATES.md` (this plan's Task 7), not resolved here. Losing Wi-Fi range still presents as heartbeat/link loss and is handled by the existing fail-safe path (§6.1, §5).
```

- [ ] **Step 10: §8 Acceptance Criteria rows**

```
Old:
| ESP32↔Phone comms | Heartbeat loss detected within 500 ms (§6.1), triggers SAFE_HOLD reliably — tested by synthetically breaking the link in sim, and on hardware by both a hard link kill *and* physically walking the phone out of AP range/into RF interference (§7 assumption 6), since wireless failure modes are broader than a wired link's |
| Phone↔EC2 comms | Robot continues safe local operation (holds last valid plan, or SAFE_HOLD if plan is stale beyond `[ASSUMPTION] 10 s`) when cloud is unreachable; GUI clearly indicates "cloud offline" |

New:
| ESP32↔EC2 comms | No valid `drive` message within 1500 ms (§6.1) forces motors to zero, reliably — tested by synthetically breaking the link against the local Mosquitto rehearsal stack (`cloud/docker-compose.yml`), and (once hardware exists) on real hardware by killing Wi-Fi, killing the broker, and an artificially throttled/delayed link (design doc §8). Once the full mode machine exists: robot continues safe local operation (holds last valid plan, or SAFE_HOLD if plan is stale beyond `[ASSUMPTION] 10 s`) when cloud is unreachable; dashboard clearly indicates "link offline." |
```

- [ ] **Step 11: §9 Latency-Budget Argument**

```
Old (final two sentences of §9):
Moving the ESP32↔phone link from wired serial to Wi-Fi (§6.1, §7) doesn't change this argument's structure — the fault→SAFE_HOLD chain is still ESP32-local and still doesn't reference the phone or cloud link's *content*, only the presence/absence of a heartbeat. It does mean that link is now less deterministic than a USB tether was, which is precisely why NFR-3 is marked `[VERIFY]` rather than assumed, and why the heartbeat-timeout mechanism (not a wired link's inherent reliability) is what's actually load-bearing for safety here.

New:
Collapsing the ESP32↔phone + phone↔EC2 two-hop design into a single ESP32↔EC2 MQTT link (§6.1, `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`) doesn't change this argument's structure — the fault→SAFE_HOLD chain is still ESP32-local and still doesn't reference the cloud link's *content*, only the presence/absence of a valid command within the watchdog window. It does mean manual-drive commands themselves now cross the same link whose latency/availability was previously isolated from them (the old phone→ESP32 hop was a separate, tighter-budget local link) — this is the one genuine safety-margin trade-off of the new design, discussed in full in the design doc's §5. NFR-4 (renamed from "Phone↔EC2") is what's now load-bearing for the drive-command path's responsiveness, not just planning/mapping data.
```

- [ ] **Step 12: Commit**

```bash
git add spec.md
git commit -m "Rewrite spec.md for ESP32-EC2-direct architecture, retiring the phone bridge"
```

---

### Task 6: overview_cloud.md, overview_controls.md, plan.md §2c

**Files:**
- Modify: `overview_cloud.md`
- Modify: `overview_controls.md`
- Modify: `plan.md` (§2c.1, §2c.2, §2c.3 steps 5-6, §2c.5)

- [ ] **Step 1: overview_cloud.md — abstract, diagram, security note**

```
Old (line 5, abstract):
A hobby-scale 4-wheel ground robot pairs a real-time ESP32 controller with a handheld Android phone bridge/GUI (connected wirelessly, not robot-mounted) and a cloud "brain" (AWS EC2 running SLAM Toolbox, Nav2, and a custom D*-Lite global planner) to deliver three operating modes — Manual, Autonomous, and Follow-me — plus an electromagnet actuator for small magnetic payload pickup. The system's defining control property is a closed-loop, autonomous incline-compensation controller: rather than relying on manual per-terrain tuning, the robot infers increased drive load from current/voltage sensing (a back-EMF-informed signal) and reallocates motor power in real time. This is enforced by a strict architectural split — the ESP32 owns all real-time control and safety locally; the cloud is used only for heavy math (mapping, global path planning) and is never in the safety-critical path.

New:
A hobby-scale 4-wheel ground robot pairs a real-time ESP32 controller, connected directly to a cloud "brain" over MQTT/TLS (AWS EC2 running SLAM Toolbox, Nav2, a custom D*-Lite global planner, the MQTT broker, and a browser-based operator dashboard — `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`), to deliver three operating modes — Manual, Autonomous, and Follow-me — plus an electromagnet actuator for small magnetic payload pickup. The system's defining control property is a closed-loop, autonomous incline-compensation controller: rather than relying on manual per-terrain tuning, the robot infers increased drive load from current/voltage sensing (a back-EMF-informed signal) and reallocates motor power in real time. This is enforced by a strict architectural split — the ESP32 owns all real-time control and safety locally; the cloud is used only for heavy math (mapping, global path planning) and is never in the safety-critical path.
```

```
Old (lines 22-28, diagram + caption):
```
ESP32 ──Wi-Fi (AP)── Phone (handheld bridge) ──MQTT/TLS── EC2 instance, single public subnet, VPC
                                                              │
                                                              ├── Mosquitto broker (TLS + auth)
                                                              ├── SLAM Toolbox  ← range scan + odom (relayed via phone)
                                                              ├── Nav2 (costmaps, controller server, lifecycle mgmt)
                                                              └── D*-Lite global planner (standalone node, see §2)
```

The scan SLAM Toolbox consumes is now assembled on the ESP32 from a single-point range sensor on a continuously sweeping servo (not a 2D lidar, dropped for cost — `spec.md` §4/§7) and relayed up through the ESP32↔phone Wi-Fi link before reaching the phone↔EC2 hop shown above; this doesn't change anything on the cloud side of the diagram, only the input rate/resolution SLAM Toolbox should be tuned to expect.

New:
```
ESP32 (Wi-Fi station) ──MQTT/TLS── EC2 instance, single public subnet, VPC
                                        │
                                        ├── Mosquitto broker (TLS + auth)
                                        ├── nginx (serves the browser dashboard)
                                        ├── SLAM Toolbox  ← range scan + odom (direct from ESP32, `[TBD]` not implemented yet)
                                        ├── Nav2 (costmaps, controller server, lifecycle mgmt)
                                        └── D*-Lite global planner (standalone node, see §2)
                                        │
                                        │  HTTPS/WSS
                                        ▼
                        Any phone/tablet/laptop (browser dashboard)
```

The scan SLAM Toolbox consumes is assembled from a single-point range sensor on a continuously sweeping servo (not a 2D lidar, dropped for cost — `spec.md` §4/§7); it now reaches EC2 as a direct ESP32→EC2 MQTT publish (`spec.md` §6.1), not relayed through a phone. This doesn't change anything else on the cloud side of the diagram, only the input rate/resolution SLAM Toolbox should be tuned to expect — and it isn't implemented yet (`spec.md` §6.1's status table), only `drive`/`telemetry` are.
```

```
Old (line 32):
Security group: MQTT(TLS) port + SSH (source-IP-restricted) only. No other inbound. IAM: a scoped least-privilege user/role, not root, not `AdministratorAccess`. Full ESP32/phone-side detail is in `overview_controls.md`; full interface contracts are in `spec.md` §6.

New:
Security group: MQTT(TLS) port + SSH (source-IP-restricted) only. No other inbound. IAM: a scoped least-privilege user/role, not root, not `AdministratorAccess`. Full ESP32-side detail is in `overview_controls.md`; full interface contracts are in `spec.md` §6.
```

```
Old (line 71):
- **Auth**: username/password or certificate-based client auth on the broker. IP-restriction alone isn't sufficient here since the phone is often on cellular with a changing IP — auth is the real control, not network position.

New:
- **Auth**: username/password or certificate-based client auth on the broker. IP-restriction alone isn't sufficient here since the ESP32 (and the dashboard's viewers) may be on a changing IP — auth is the real control, not network position.
```

```
Old (line 73):
- **The ESP32↔phone hop is a separate wireless surface from anything in this section**, and shouldn't be overlooked just because it's not cloud-side: the ESP32 now hosts its own Wi-Fi AP (`spec.md` §6.1, since the phone is handheld rather than robot-mounted) and must require WPA2/WPA3 auth. An open AP there would let anyone in range inject `mode_request`/`estop`/`magnet_trigger` commands directly at the ESP32 — upstream of, and independent from, the MQTT broker auth described above.

New:
- **There is no longer a separate ESP32↔phone wireless surface to secure independently** — the old local-AP design (`[REMOVED]`, see `spec.md` §7 assumption 4) had its own WPA2/WPA3 auth requirement distinct from the broker's; now the ESP32's only wireless hop *is* the MQTT/TLS link to the broker, so the auth/TLS requirements above are the whole story, not half of it.
```

```
Old (line 75):
- **Why this matters concretely**: an unauthenticated, internet-reachable broker would let anyone who finds the port inject fake telemetry, fake waypoints, or — more seriously — a fake `magnet_trigger` or spoofed mode-change command. Because the ESP32 is architected to never trust the cloud link for safety (`overview_controls.md` §3–4), a compromised broker can't directly cause an unsafe *physical* action outside of what the phone/ESP32 would otherwise accept as a normal command — but it could still cause nuisance behavior (unwanted electromagnet triggers, corrupted maps, bogus waypoints) and should not be treated as low-stakes just because it isn't in the direct safety path.

New:
- **Why this matters concretely**: an unauthenticated, internet-reachable broker would let anyone who finds the port inject fake telemetry, fake waypoints, or — more seriously — a fake `magnet_trigger`, spoofed mode-change command, or (now that drive commands ride this same link) a fake `drive` command. Because the ESP32 is architected to never trust the cloud link for safety (`overview_controls.md` §3–4) beyond the bounded watchdog-timeout behavior, a compromised broker can't keep the robot moving past the 1500ms watchdog window on its own — but it could still cause nuisance or startling behavior (unwanted electromagnet triggers, corrupted maps, bogus waypoints, a burst of fake drive commands) and should not be treated as low-stakes just because it isn't in the fault→SAFE_HOLD path.
```

- [ ] **Step 2: overview_controls.md — abstract, diagram, mode/task-structure/cloud-summary references**

```
Old (line 5, abstract — identical text to overview_cloud.md's old line 5):
A hobby-scale 4-wheel ground robot pairs a real-time ESP32 controller with a handheld Android phone bridge/GUI (connected wirelessly, not robot-mounted) and a cloud "brain" (AWS EC2 running SLAM Toolbox, Nav2, and a custom D*-Lite global planner) to deliver three operating modes — Manual, Autonomous, and Follow-me — plus an electromagnet actuator for small magnetic payload pickup. The system's defining control property is a closed-loop, autonomous incline-compensation controller: rather than relying on manual per-terrain tuning, the robot infers increased drive load from current/voltage sensing (a back-EMF-informed signal) and reallocates motor power in real time. This is enforced by a strict architectural split — the ESP32 owns all real-time control and safety locally; the cloud is used only for heavy math (mapping, global path planning) and is never in the safety-critical path.

New: (same replacement text as overview_cloud.md Step 1's abstract edit above — keep the two files' shared abstract in sync, per that file's own "shares this same abstract" note)
A hobby-scale 4-wheel ground robot pairs a real-time ESP32 controller, connected directly to a cloud "brain" over MQTT/TLS (AWS EC2 running SLAM Toolbox, Nav2, a custom D*-Lite global planner, the MQTT broker, and a browser-based operator dashboard — `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`), to deliver three operating modes — Manual, Autonomous, and Follow-me — plus an electromagnet actuator for small magnetic payload pickup. The system's defining control property is a closed-loop, autonomous incline-compensation controller: rather than relying on manual per-terrain tuning, the robot infers increased drive load from current/voltage sensing (a back-EMF-informed signal) and reallocates motor power in real time. This is enforced by a strict architectural split — the ESP32 owns all real-time control and safety locally; the cloud is used only for heavy math (mapping, global path planning) and is never in the safety-critical path.
```

```
Old (lines 22-32, diagram + caption):
```
   ESP32 (real-time control + safety)  ◄──Wi-Fi, UDP JSON──►  Phone (handheld bridge/GUI)
        │  IMU, GPS, current/voltage,                              │
        │  motor PWM, electromagnet,                                │
        │  range sensor + sweep servo                                │
        └── owns: mode arbitration, SAFE_HOLD,                     └── MQTT/TLS ──► EC2
            incline-compensation loop, stall/                          (SLAM Toolbox,
            brownout protection, scan assembly                         Nav2, D*-Lite —
                                                                         heavy math only)
```

The ESP32 now hosts its own Wi-Fi access point — the phone is a handheld device, not robot-mounted, and connects to it directly (`spec.md` §6.1, §7). This is also why the range sensor moved from the phone (formerly USB-OTG) onto the ESP32: a single-point sensor on a continuously sweeping servo, emulating a 2D scan (§2 sensor list in `spec.md`, not a spinning 2D lidar — dropped for cost).

New:
```
   ESP32 (real-time control + safety)  ──Wi-Fi station, MQTT/TLS──►  EC2
        │  IMU, GPS, current/voltage,                                 (SLAM Toolbox,
        │  motor PWM, electromagnet,                                   Nav2, D*-Lite,
        │  range sensor + sweep servo                                  Mosquitto broker,
        └── owns: mode arbitration, SAFE_HOLD,                         browser dashboard —
            incline-compensation loop, stall/                          heavy math + GUI only)
            brownout protection, scan assembly
```

The ESP32 is a Wi-Fi *station*, not an access point — it joins local Wi-Fi and talks MQTT/TLS directly to EC2 (`spec.md` §6.1, §7; `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`). There is no phone in this architecture anymore; the browser dashboard served from EC2 is a client of the same broker, not a bridge the ESP32 connects to directly. The range sensor remains on the ESP32 (not on any operator device): a single-point sensor on a continuously sweeping servo, emulating a 2D scan (§2 sensor list in `spec.md`, not a spinning 2D lidar — dropped for cost).
```

```
Old (line 78):
Entirely on the ESP32, at the control loop's native rate (`spec.md` NFR-1, `[ASSUMPTION] 100–200 Hz`). No part of this loop reads from or waits on the phone or EC2 link — it is local reflex behavior by definition (see §3).

New:
Entirely on the ESP32, at the control loop's native rate (`spec.md` NFR-1, `[ASSUMPTION] 100–200 Hz`). No part of this loop reads from or waits on the EC2 link — it is local reflex behavior by definition (see §3).
```

```
Old (line 89, table row):
| Input | IMU, GPS, current/voltage, heartbeat state, range sensor + sweep servo (raw samples) | assembled range scan (relayed from ESP32 via phone), odometry, goal |

New:
| Input | IMU, GPS, current/voltage, heartbeat state, range sensor + sweep servo (raw samples) | assembled range scan (published directly by ESP32, `[TBD]` not implemented yet), odometry, goal |
```

```
Old (line 97):
Full transition table is in `spec.md` §5. The controls-relevant point: the ESP32 is the **sole authority** over the confirmed mode, not the phone GUI — because mode arbitration is itself a safety decision (e.g., refusing to enter Autonomous without a fresh valid plan), and safety decisions can't be delegated to a non-real-time, non-safety-rated bridge device. On any mode exit, in-flight commands (queued waypoints, a Follow-me target lock) are discarded, and the new mode starts from a **zero** command state rather than continuing the previous mode's last command — this avoids a surprise-motion failure mode where, e.g., switching out of Autonomous mid-turn continues that turn under manual control before the operator has given fresh input.

New:
Full transition table is in `spec.md` §5. The controls-relevant point: the ESP32 is the **sole authority** over the confirmed mode, not the browser dashboard — because mode arbitration is itself a safety decision (e.g., refusing to enter Autonomous without a fresh valid plan), and safety decisions can't be delegated to a non-real-time, non-safety-rated client. On any mode exit, in-flight commands (queued waypoints, a Follow-me target lock) are discarded, and the new mode starts from a **zero** command state rather than continuing the previous mode's last command — this avoids a surprise-motion failure mode where, e.g., switching out of Autonomous mid-turn continues that turn under manual control before the operator has given fresh input.
```

```
Old (line 107):
- **Task structure**: a high-priority control task (sensor sampling, incline-compensation loop, mode/SAFE_HOLD logic) should run at a fixed, guaranteed rate, separated from lower-priority tasks like phone-link JSON parsing (now over a Wi-Fi socket rather than a UART, per `spec.md` §6.1) — so a slow or malformed packet parse can't introduce jitter into the control loop. `[VERIFY]` ESP-IDF's FreeRTOS supports pinning tasks to specific cores on dual-core variants, which — if the chosen variant is dual-core — is a reasonable way to physically isolate the control task from comms handling; confirm against the specific variant chosen `[TBD]`, since some ESP32 variants are single-core.

New:
- **Task structure**: a high-priority control task (sensor sampling, incline-compensation loop, mode/SAFE_HOLD logic) should run at a fixed, guaranteed rate, separated from lower-priority tasks like MQTT message parsing/reconnect handling (`spec.md` §6.1) — so a slow parse, a malformed payload, or a blocking broker-reconnect attempt can't introduce jitter (or worse, a stuck loop) into the control loop. `[VERIFY]` ESP-IDF's FreeRTOS supports pinning tasks to specific cores on dual-core variants, which — if the chosen variant is dual-core — is a reasonable way to physically isolate the control task from comms handling; confirm against the specific variant chosen `[TBD]`, since some ESP32 variants are single-core.
```

```
Old (line 116):
EC2 runs SLAM Toolbox (mapping from relayed range scan + odometry — the scan assembled on the ESP32 from a servo-swept single-point sensor, not a 2D lidar) and Nav2 with a custom D*-Lite global planner (`plan.md` 2a.4), reachable over MQTT/TLS from the phone bridge. The whole stack is validated locally (Dockerized, same broker software, same message contracts) before any AWS resource exists, per the project's hard constraint. Full AWS topology, cost, and security detail is in `overview_cloud.md`.

New:
EC2 runs SLAM Toolbox (mapping from range scan + odometry published directly by the ESP32, `[TBD]` not implemented yet — the scan assembled from a servo-swept single-point sensor, not a 2D lidar) and Nav2 with a custom D*-Lite global planner (`plan.md` 2a.4), plus the Mosquitto broker and the browser dashboard's static files. The whole stack is validated locally (Dockerized, same broker software, same message contracts) before any AWS resource exists, per the project's hard constraint. Full AWS topology, cost, and security detail is in `overview_cloud.md`.
```

- [ ] **Step 3: plan.md §2c.1, §2c.2, §2c.3 steps 5-6, §2c.5**

```
Old (§2c.1 body):
Because 2a and 2b deliberately share the same broker software and topic/payload contracts (`spec.md` §6.2, plan.md 2b.1), this swap should be limited to: (1) changing the MQTT broker hostname/port/credentials the phone app points at, from the local Mosquitto container to the EC2 instance's address, and (2) enabling TLS + auth on the client side to match the now-mandatory server-side security (2b.3). No topic, payload, or node-logic changes should be required — if a change beyond configuration turns out to be necessary, that's a signal the 2a/2b interface parity wasn't actually maintained and should be fixed rather than patched around.

New:
Because 2a and 2b deliberately share the same broker software and topic/payload contracts (`spec.md` §6.1, plan.md 2b.1), this swap should be limited to: (1) changing the MQTT broker hostname/port/credentials the ESP32 (`embedded/web_drive/mqtt_handler.h`'s `MQTT_BROKER_HOST`/`MQTT_BROKER_PORT`) and the dashboard (`cloud/dashboard/app.js`'s `MQTT_WS_URL`) point at, from the local Mosquitto container to the EC2 instance's address, and (2) enabling TLS + auth on both clients to match the now-mandatory server-side security (2b.3, design D4/D5). No topic, payload, or node-logic changes should be required — if a change beyond configuration turns out to be necessary, that's a signal the 2a/2b interface parity wasn't actually maintained and should be fixed rather than patched around.
```

```
Old (§2c.2 heading + body):
### 2c.2 Phone-to-cloud networking

- **Cellular vs. local Wi-Fi**: cellular gives the robot mobility independent of a fixed Wi-Fi network (important if the robot is meant to operate outdoors/away from a home network), but has more variable and generally higher latency, potential carrier-side NAT/firewall behavior, and data cost considerations. Local Wi-Fi gives lower/more consistent latency but ties operation to Wi-Fi coverage range. `[ASSUMPTION]` design for cellular as the general case (since a ground robot's whole point is mobility) and treat Wi-Fi as a lower-latency special case that works the same way — this is exactly why NFR-4's latency budget in `spec.md` was deliberately left wide (100 ms–2000+ ms) rather than tightly bounded, and why nothing safety-relevant may depend on it (§9).
- **MQTT vs. WebSocket**: MQTT was chosen in `spec.md` §6.2 specifically for its reconnect/session/QoS semantics, which matter more over a cellular link with intermittent connectivity than over a stable local network. This choice should be re-validated once real cellular behavior is observed — if reconnect behavior under real carrier NAT turns out to be worse than expected, that's a concrete, testable reason to revisit, not a reason to preemptively switch now.

New:
### 2c.2 ESP32-to-cloud networking

- **Cellular vs. local Wi-Fi**: in the old phone-bridge design, the *phone* carried the cellular radio and the robot's outdoor mobility rode on the operator's phone plan. Since the 2026-10-04 cloud-dashboard design removes the phone, **the ESP32 itself is now the device that needs internet reachability**, and it has no cellular radio in the current sensor list (`spec.md` §4, `ESP32 variant` still `[TBD]`). Outdoor/away-from-fixed-Wi-Fi operation now requires either an external Wi-Fi hotspot carried near the robot, or a cellular-capable ESP32 variant/add-on not currently planned. This is a new open question flagged in `UPDATES.md`, not resolved by this plan.
- **MQTT vs. WebSocket**: unchanged reasoning from before — MQTT's reconnect/session/QoS semantics still matter more over an intermittent link than over a stable local network, now for the ESP32's own link rather than the phone's.
```

```
Old (§2c.3 steps 5-6):
5. Only after 1–4 pass individually: bring up the ESP32's Wi-Fi AP and confirm the phone can associate, authenticate (WPA2/WPA3, `spec.md` §6.1), and exchange the JSON-over-UDP contract at expected latency (NFR-3) — then re-run the full mode-transition matrix from `spec.md` §5 against **real** hardware, the same test suite already proven in sim (2a.5).
6. Only after 5 passes: point the phone at the real EC2 endpoint (2c.1) and re-run the Autonomous/Follow-me exercises end-to-end.

New:
5. Only after 1–4 pass individually: bring up the ESP32's Wi-Fi station connection and MQTT client (`embedded/web_drive/`, design doc `2026-10-04-cloud-web-dashboard-design.md`) against the **local** Mosquitto rehearsal stack (`cloud/docker-compose.yml`) first — confirm Wi-Fi association, MQTT connect/subscribe/publish, and the command watchdog (1500ms) all work on real hardware before any cloud endpoint is involved. Once the full mode state machine exists on the ESP32 (not yet — see `spec.md` §6.1's implementation-status table), re-run the full mode-transition matrix from `spec.md` §5 against real hardware here too, the same test suite already proven in sim (2a.5).
6. Only after 5 passes on the local broker: point the ESP32 at the real EC2 endpoint (2c.1) and re-run the same checks end-to-end. Once Autonomous/Follow-me exist on the ESP32 (not yet), re-run those exercises here too.
```

```
Old (§2c.5 bullet):
- **ESP32↔phone comms loss, real hardware**: re-validate the exact scenario proven in sim (2a.6) on real hardware, now with a broader set of real causes than a wired link had — power off the phone's Wi-Fi during Autonomous/Follow-me, walk the phone out of AP range, and (if feasible) introduce 2.4/5 GHz interference (e.g. another AP or a microwave-class source) — and confirm SAFE_HOLD entry within the NFR-2 budget in each case, not just in sim timing. Also confirm the AP requires WPA2/WPA3 auth in the field build, not a dev-convenience open AP left over from bring-up.

New:
- **ESP32↔EC2 comms loss, real hardware**: re-validate the exact scenario proven in sim (2a.6) on real hardware. Causes to test: Wi-Fi disconnect, broker connection drop, and (per the design doc's own bring-up recommendation) an artificially throttled/delayed link — confirm the 1500ms watchdog reliably zeroes drive output in each case, not just in sim timing. Once SAFE_HOLD exists on the ESP32 (not yet, see `spec.md` §6.1), confirm SAFE_HOLD entry within the NFR-2 budget too. Also confirm the broker requires TLS+auth in the field build, not a dev-convenience open/unauthenticated local config left over from bring-up (`cloud/mosquitto/mosquitto.conf` is explicitly dev-only, see its header comment).
```

```
Old (§2c.5 latency bullet, one phrase):
...this is the phase where it's most tempting to accidentally violate it — e.g. by having the phone app wait on an EC2 acknowledgment before forwarding an e-stop press, or by routing any part of the SAFE_HOLD path through the cloud "just for logging."...

New:
...this is the phase where it's most tempting to accidentally violate it — e.g. by having the dashboard wait on an EC2 acknowledgment before forwarding an e-stop press, or by routing any part of the SAFE_HOLD path through the cloud "just for logging."...
```

- [ ] **Step 4: Commit**

```bash
git add overview_cloud.md overview_controls.md plan.md
git commit -m "Sync overview_cloud.md, overview_controls.md, and plan.md §2c to the ESP32-EC2-direct architecture"
```

---

### Task 7: UPDATES.md — resolve blocker #8, flag follow-ups

**Files:**
- Modify: `UPDATES.md`

- [ ] **Step 1: Move blocker #8 to "Recently resolved"**

```
Old (Open blockers table row #8):
| 8 | **Architecture change, user-directed**: `spec.md` §6.1 (ESP32↔Phone UDP) and §6.2 (Phone↔EC2 MQTT) are proposed to be replaced by a single direct ESP32↔EC2 MQTT/TLS link — ESP32 becomes a Wi-Fi station (drops its own AP), phone-as-bridge role is removed, and the planned Android app is replaced by a browser dashboard hosted on EC2. Design: `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md` (pending user review, not yet implemented, no AWS resource created). | Embedded engineer — your ESP32 Wi-Fi-AP + UDP server work against the current §6.1 shape would be obsoleted by this if approved. **Do not build against current §6.1 until this resolves** | Cloud/GUI engineer (this session) + human sign-off | 2026-10-04 |

New: (remove this row from the Open blockers table — it's resolved)
```

Add to the "Recently resolved" table:

```
| 8 | Architecture change: `spec.md` §6.1/§6.2 (ESP32↔Phone↔EC2 two-hop) replaced by direct ESP32↔EC2 MQTT/TLS; phone-bridge role and the planned Android app are cancelled, replaced by a browser dashboard hosted on EC2. | User approved all of design decisions D1–D7 on 2026-10-04 (`docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`). Implemented locally only (no AWS resource created) per `docs/superpowers/plans/2026-10-04-cloud-web-dashboard.md`: `embedded/web_drive/` ported to Wi-Fi-station+MQTT (this session, explicit one-off exception — see that plan's role-boundary note), `cloud/dashboard/` is the new MQTT-over-WebSocket frontend, `cloud/docker-compose.yml` is the local Mosquitto rehearsal stack. `spec.md`, `overview_cloud.md`, `overview_controls.md`, and `plan.md` §2c updated to match. | 2026-10-04 |
```

- [ ] **Step 2: Add a new blocker for physical bench bring-up**

Add to the Open blockers table:

```
| 9 | New MQTT-based `embedded/web_drive/` firmware + `cloud/dashboard/` frontend (`docs/superpowers/plans/2026-10-04-cloud-web-dashboard.md`) is written and passes local-rehearsal checks against a Dockerized Mosquitto broker, but **has not been bench-tested on real ESP32 hardware** — same situation as the original web_drive bring-up tool (`UPDATES.md` #7's resolution). Needs: wheels-off-the-ground bring-up, watchdog-trip verification on a real link, and telemetry cross-check against `drive_test.ino`'s known-good values (full checklist in the design doc's §8 and the plan's Task 4 Step 4). | Whoever's at the bench next — hardware bring-up isn't role-exclusive the way firmware authorship is (see `ONBOARDING.md` §1's physical-hardware note) | Whoever does the bench bring-up | 2026-10-04 |
| 10 | Dropping the phone bridge means the ESP32 itself (not the phone) now needs internet reachability for the full robot, and the ESP32 has no cellular radio in the current sensor list (`spec.md` §4, `ESP32 variant` still `[TBD]`) — outdoor/away-from-fixed-Wi-Fi operation needs either a carried Wi-Fi hotspot or a cellular-capable ESP32 variant/add-on, neither currently planned (`spec.md` §7 assumption 6, `plan.md` §2c.2). | Both — affects the embedded engineer's eventual ESP32-variant BOM choice and the cloud/GUI engineer's assumptions about field connectivity | Human (BOM/connectivity decision, same category as blocker #3) | 2026-10-04 |
| 11 | The MQTT-based redesign has no per-topic client arbitration — the old local-AP design allowed at most one WebSocket client at a time (`wsClientFd`); `robot/{id}/drive` now accepts publishes from any connected client with no "who's in control" concept, so two open dashboard tabs/devices would silently fight over the same topic. Not fixed by `docs/superpowers/plans/2026-10-04-cloud-web-dashboard.md` (out of its v1 scope, design D7) — flagging so it isn't a silent regression. | Cloud/GUI engineer, whenever multi-operator/multi-tab use becomes a real scenario (not v1) | Cloud/GUI engineer | 2026-10-04 |
| 12 | `plan.md` §2a's local-sim architecture (§2a.2/2a.3/2a.5/2a.6 — the Gazebo "simulated-phone" ROS2 node) still describes the old ESP32↔phone JSON-over-UDP contract and wasn't updated by `docs/superpowers/plans/2026-10-04-cloud-web-dashboard.md` (out of that plan's scope — it only touched `plan.md` §2c). Needs its own pass once SLAM/Nav2 sim work actually starts. | Cloud/GUI engineer (2a is cloud/GUI-owned sim work) | Cloud/GUI engineer | 2026-10-04 |
```

- [ ] **Step 3: Commit**

```bash
git add UPDATES.md
git commit -m "Resolve UPDATES.md blocker 8, flag bench bring-up and follow-on consequences"
```

---

## Self-Review Notes (for whoever executes this plan)

- **Spec coverage**: design doc §1 (scope) → Tasks 1-4; §2 D1-D7 → reflected throughout Tasks 4-6's doc text and firmware; §3 architecture → Task 1 (compose topology) + Task 5 Step 5 (spec diagram); §4 protocol → Task 2 (topics/shapes) + Task 4 (firmware) + Task 5 Step 7 (spec table); §5 safety → Task 4's `mqttReconnect()` stopAll-before-blocking-connect design + Review Focus items 1-2; §6 AWS setup → explicitly NOT done (Scope boundary section) pending separate human go-ahead; §7 file layout → File Structure/Files blocks in Tasks 1-4; §8 testing plan → item 1 is Task 1/3/4's local rehearsal, items 2-6 are Task 4 Step 4's bench checklist + Task 7's new blocker #9; §9 open assumptions → carried into `spec.md`'s `[TBD]`/`[ASSUMPTION]` tags in Task 5, plus new blockers #10/#11.
- **No AWS resource is created anywhere in this plan** — verified by re-reading every task; Tasks 1/3/4 all target `localhost`/the Docker network only.
- **ROBOT_ID consistency**: `"robot1"` appears in `cloud/dashboard/mqtt_topics.js` (Task 2), `embedded/web_drive/mqtt_handler.h` (Task 4), and `spec.md`'s `[ASSUMPTION]` note (Task 5) — kept as a plain duplicated string literal per the Global Constraints note, not a shared generated constant.
