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

// Command watchdog: intended to run every loop() iteration regardless of
// MQTT connection state - this is the ONLY thing that stops the motors on a
// dropped link, same role as the local-AP version's wsWatchdogCheck(). See
// the MQTT_SOCKET_TIMEOUT note in mqttSetup() below for the one case where
// a blocking library call can still delay this past WATCHDOG_MS.
static void mqttWatchdogCheck() {
  if (millis() - lastDriveMsgMs > WATCHDOG_MS) {
    stopAll();
  }
}

static void mqttSetup() {
  snprintf(driveTopicBuf, sizeof(driveTopicBuf), "robot/%s/drive", ROBOT_ID);
  snprintf(telemetryTopicBuf, sizeof(telemetryTopicBuf), "robot/%s/telemetry", ROBOT_ID);

  // PubSubClient's default socket timeout is 15s: if a drive packet starts
  // arriving and then the link dies mid-packet, mqttClient.loop() (called
  // from mqttLoop() below) blocks inside that read for up to 15s - during
  // which mqttWatchdogCheck() above does NOT run, so the motors could keep
  // their last commanded duty for far longer than WATCHDOG_MS (1500ms).
  // Capping the socket timeout at 1s bounds that stall to roughly 1s
  // instead of 15s. This does not make the watchdog's "every iteration"
  // guarantee absolute - a fully robust version would run the watchdog
  // from a separate timer/task so loop() blocking can never affect it -
  // but it closes the gap enough for this bring-up tool, and the 1500ms
  // budget is dominated by this 1s bound now rather than the library
  // default. Bench-check this specifically (partial-packet / mid-drop
  // scenario) per UPDATES.md blocker #9.
  mqttClient.setSocketTimeout(1);
  // Also shortens how quickly a half-dead TCP connection (broker process
  // hung but socket not yet reset) is detected and dropped, rather than
  // waiting out the 15s default MQTT keepalive.
  mqttClient.setKeepAlive(5);

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
