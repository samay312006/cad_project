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
// JSON, isn't an object, is missing a required field, or has a required
// field of the wrong type. The type check matters as much as the presence
// check: renderTelemetry() calls msg.vbus.toFixed(1) directly, so a string/
// null/object vbus would throw (or render "[object Object]"/"NaN") instead
// of being dropped here. Never throws - callers (app.js's MQTT message
// handler) can treat null as "drop this message" without a try/catch of
// their own.
function parseTelemetryMessage(payload) {
  var msg;
  try { msg = JSON.parse(payload); } catch (e) { return null; }
  if (typeof msg !== "object" || msg === null || Array.isArray(msg)) return null;
  var required = ["left_a", "right_a", "left_duty", "right_duty", "vbus"];
  for (var i = 0; i < required.length; i++) {
    var v = msg[required[i]];
    if (typeof v !== "number" || !isFinite(v)) return null;
  }
  return msg;
}

// True if the robot should be considered offline from the dashboard's point
// of view: either no telemetry has ever arrived (lastMs === null) or none
// has arrived within timeoutMs. This is deliberately separate from the MQTT
// client's own connected/disconnected state - "connected to the broker"
// only means the browser can reach the broker, not that the ESP32 is
// reachable, powered, or on Wi-Fi. Without this check the dashboard would
// show a stale "connected" ring and the last-known telemetry forever if the
// robot itself drops while the broker stays up.
function isTelemetryStale(lastMs, nowMs, timeoutMs) {
  if (lastMs === null) return true;
  return (nowMs - lastMs) > timeoutMs;
}

if (typeof module !== "undefined") {
  module.exports = {
    ROBOT_ID: ROBOT_ID,
    driveTopic: driveTopic,
    telemetryTopic: telemetryTopic,
    buildDriveMessage: buildDriveMessage,
    parseTelemetryMessage: parseTelemetryMessage,
    isTelemetryStale: isTelemetryStale,
  };
}
