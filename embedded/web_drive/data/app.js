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
      if (dragging) startKeepalive();
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
