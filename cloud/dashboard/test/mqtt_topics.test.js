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
