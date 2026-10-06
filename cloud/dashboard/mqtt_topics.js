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
