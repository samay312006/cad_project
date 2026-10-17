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

#include "secrets.h"
#include "drive_core.h"
#include "mqtt_handler.h"

// Wi-Fi credentials come from the uncommitted secrets.h (see
// secrets.h.example). 2.4 GHz network with internet access required.
static const char *WIFI_SSID     = WIFI_SSID_S;
static const char *WIFI_PASSWORD = WIFI_PASS_S;

static uint32_t telemetryNextMs = 0;
static const uint32_t TELEMETRY_MS = 200;   // ~5 Hz, unchanged from the local-AP version

void setup() {
  Serial.begin(115200);
  delay(300);

  driveSetup();   // kills both bridges first, then brings up pins + INA219s

  WiFi.mode(WIFI_STA);
  // Modem-sleep (the default) holds incoming frames until the next DTIM
  // beacon, which measured ~1s extra one-way latency on the drive path over
  // the real link (UPDATES #9 bench). The robot is mains/battery-powered
  // and the watchdog budget is tight, so trade the power saving for latency.
  WiFi.setSleep(false);
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
