/*
 * web_drive.ino - landscape browser drive control + telemetry, served
 * directly from the ESP32 over its own Wi-Fi AP.
 *
 * BRING-UP TOOL, not the real firmware: no SAFE_HOLD, no mode state machine
 * (spec.md §5). Two things stand in for that here:
 *   - a voltage-based duty clamp (drive_core.h, from drive_calib.ino) so
 *     3.0-6.0V motors never see more than ~5.5V from this 12V bus
 *   - a 400ms command watchdog (ws_handler.h): no valid "drive" message ->
 *     motors stop
 *
 * WHEELS OFF THE GROUND for first bring-up. Full design:
 * docs/superpowers/specs/2026-09-03-web-drive-gui-design.md
 *
 * Requires a LittleFS image built from this sketch's data/ folder
 * (index.html/style.css/mix.js/app.js) uploaded before first use - e.g.
 * Arduino IDE "ESP32 Sketch Data Upload" tool, or the equivalent for your
 * IDE version.
 */

#include <WiFi.h>
#include <ESPmDNS.h>

#include "drive_core.h"
#include "ws_handler.h"

static const char *AP_SSID     = "RobotDrive";
static const char *AP_PASSWORD = "drive1234";   // WPA2, 8+ chars - change before field use

static uint32_t telemetryNextMs = 0;
static const uint32_t TELEMETRY_MS = 200;   // ~5 Hz

void setup() {
  Serial.begin(115200);
  delay(300);

  driveSetup();   // kills both bridges first, then brings up pins + INA219s

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP started: ");
  Serial.println(WiFi.softAPIP());

  if (MDNS.begin("robotdrive")) {
    Serial.println("mDNS started: http://robotdrive.local/");
  } else {
    Serial.println("mDNS failed to start");
  }

  if (!startWebServer()) {
    Serial.println("Web server failed to start - halting");
    while (true) { delay(1000); }
  }
  Serial.println("Web server up. Open http://<AP IP>/ or http://robotdrive.local/");
}

void loop() {
  wsWatchdogCheck();

  if ((int32_t)(millis() - telemetryNextMs) >= 0) {
    telemetryNextMs = millis() + TELEMETRY_MS;
    wsSendTelemetry();
  }
}
