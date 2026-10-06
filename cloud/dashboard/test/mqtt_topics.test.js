// cloud/dashboard/test/mqtt_topics.test.js
const assert = require("assert");
const { driveTopic, telemetryTopic, buildDriveMessage, parseTelemetryMessage, isTelemetryStale } = require("../mqtt_topics.js");

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

// parseTelemetryMessage: a required field present but the wrong type (string,
// null, object, array, NaN-producing) must be rejected, not rendered -
// otherwise renderTelemetry()'s msg.vbus.toFixed(1) throws on a string vbus,
// or prints "[object Object]"/"NaN" on the dashboard for an object/array.
{
  const base = { left_a: 0.1, right_a: 0.1, left_duty: 1, right_duty: 1, vbus: 12.0 };
  assert.strictEqual(parseTelemetryMessage(JSON.stringify({ ...base, vbus: "12.0" })), null);
  assert.strictEqual(parseTelemetryMessage(JSON.stringify({ ...base, vbus: null })), null);
  assert.strictEqual(parseTelemetryMessage(JSON.stringify({ ...base, left_duty: {} })), null);
  assert.strictEqual(parseTelemetryMessage(JSON.stringify({ ...base, left_duty: [1] })), null);
  assert.strictEqual(parseTelemetryMessage(JSON.stringify({ ...base, left_a: "a" })), null);
}

// isTelemetryStale: no telemetry received yet (lastMs === null) is stale -
// the dashboard must not imply "robot is live" before any telemetry arrives.
{
  assert.strictEqual(isTelemetryStale(null, 1000, 1000), true);
}

// isTelemetryStale: within the timeout window is fresh
{
  assert.strictEqual(isTelemetryStale(1000, 1500, 1000), false);
}

// isTelemetryStale: exactly at the timeout boundary is still fresh (strictly greater, not >=)
{
  assert.strictEqual(isTelemetryStale(1000, 2000, 1000), false);
}

// isTelemetryStale: past the timeout window is stale - this is the case that
// catches the robot going offline (Wi-Fi/broker/ESP32 power loss) even while
// the dashboard's own MQTT connection to the broker stays up, which the
// broker-connection-only "connected" flag can't detect on its own
{
  assert.strictEqual(isTelemetryStale(1000, 2001, 1000), true);
}

console.log("mqtt_topics.test.js: all assertions passed");
