#pragma once

// secrets.example.h - committed TEMPLATE for web_drive's per-deployment
// settings. Copy it to secrets.h (same folder, gitignored) and fill in real
// values there; never put real credentials in this file. web_drive.ino uses
// secrets.h when it exists and falls back to these placeholders otherwise
// (with a compile-time warning), so a fresh clone still builds.

// ---- Wi-Fi (station mode) ----
static const char *WIFI_SSID     = "your-dev-wifi";
static const char *WIFI_PASSWORD = "your-dev-wifi-password";

// ---- MQTT broker ----
// Local bench: the dev machine's LAN IP running a Mosquitto broker, plain
// TCP (MQTT_USE_TLS 0, port 1883). The ESP32 can't resolve "localhost" to
// the dev machine - it needs a real reachable address on the same Wi-Fi.
// EC2: the Elastic IP, TLS (MQTT_USE_TLS 1, port 8883), design D2/D4.
#define MQTT_USE_TLS 0
static const char *MQTT_BROKER_HOST = "192.0.2.1";
static const uint16_t MQTT_BROKER_PORT = 1883;

// Broker username/password (Mosquitto password_file). Leave both empty for
// an anonymous bench broker; the EC2 broker rejects anonymous clients.
static const char *MQTT_USERNAME = "";
static const char *MQTT_PASSWORD = "";

// PEM of the project CA that signed the broker's server certificate (only
// used when MQTT_USE_TLS is 1). Pinning the CA rather than the server cert
// means re-issuing the server cert (e.g. a new Elastic IP) needs no
// firmware change. The server cert's subjectAltName must contain the exact
// MQTT_BROKER_HOST value (IP:... for an address) - the handshake checks it.
static const char *MQTT_CA_CERT = R"PEM(
-----BEGIN CERTIFICATE-----
...paste the project CA certificate here...
-----END CERTIFICATE-----
)PEM";
