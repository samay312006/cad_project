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
