#pragma once

// ws_handler.h - esp_http_server: static file serving from LittleFS, the
// /ws WebSocket endpoint (drive commands in, telemetry out), and the
// 400ms command watchdog. Depends on drive_core.h being included first.

#include <esp_http_server.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>

static httpd_handle_t server = NULL;
static int wsClientFd = -1;
static uint32_t lastDriveMsgMs = 0;
static const uint32_t WATCHDOG_MS = 400;

// ---- inbound: {"type":"drive","left":N,"right":N} ----
// Hand-rolled parsing, not a JSON library: the message shape is fixed and
// produced only by our own frontend (data/app.js), so this is sufficient
// and avoids a new library dependency (design D6's minimal-dependency
// spirit extended to the wire format). A malformed or out-of-range message
// returns false and is dropped silently - the watchdog is NOT reset, so a
// garbled command degrades to "eventually stops," never "drives on bad
// data."
static bool parseDriveMessage(const char *json, int *left, int *right) {
  const char *lp = strstr(json, "\"left\"");
  const char *rp = strstr(json, "\"right\"");
  if (!lp || !rp) return false;

  lp = strchr(lp, ':');
  rp = strchr(rp, ':');
  if (!lp || !rp) return false;

  long l = strtol(lp + 1, nullptr, 10);
  long r = strtol(rp + 1, nullptr, 10);
  if (l < -255 || l > 255 || r < -255 || r > 255) return false;

  *left  = (int)l;
  *right = (int)r;
  return true;
}

static esp_err_t wsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    // Handshake call - no frame to read yet.
    wsClientFd = httpd_req_to_sockfd(req);
    Serial.println("WS client connected");
    return ESP_OK;
  }

  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type = HTTPD_WS_TYPE_TEXT;

  // First call with a NULL/zero-length buffer just reports pkt.len.
  esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
  if (ret != ESP_OK) return ret;
  if (pkt.len == 0) return ESP_OK;

  static uint8_t buf[257];
  size_t recvLen = pkt.len;
  if (recvLen >= sizeof(buf)) recvLen = sizeof(buf) - 1;  // truncate defensively

  pkt.payload = buf;
  ret = httpd_ws_recv_frame(req, &pkt, recvLen);
  if (ret != ESP_OK) return ret;

  size_t n = pkt.len;
  if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
  buf[n] = '\0';

  if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
    Serial.println("WS client disconnected");
    wsClientFd = -1;
    stopAll();
    return ESP_OK;
  }

  if (pkt.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

  int left, right;
  if (parseDriveMessage((const char *)buf, &left, &right)) {
    drive(left, right);
    lastDriveMsgMs = millis();
  }
  return ESP_OK;
}

// Command watchdog: call every loop() iteration. No valid drive message
// within WATCHDOG_MS -> force stop. Covers both a clean disconnect (handled
// above too, for a faster stop) and a silent connection loss (stalled Wi-Fi,
// crashed browser tab) where no CLOSE frame ever arrives.
static void wsWatchdogCheck() {
  if (millis() - lastDriveMsgMs > WATCHDOG_MS) {
    stopAll();
  }
}

// Push telemetry to the connected client, if any. Uses the async send path
// (needs only the socket fd, not a live httpd_req_t) since this is called
// from loop(), not from inside a request handler.
static void wsSendTelemetry() {
  if (wsClientFd < 0) return;

  readCurrents();

  char json[192];
  int n = snprintf(json, sizeof(json),
    "{\"type\":\"telemetry\",\"left_a\":%.3f,\"right_a\":%.3f,"
    "\"left_duty\":%d,\"right_duty\":%d,\"vbus\":%.2f}",
    isense[LEFT].amps, isense[RIGHT].amps,
    lastDuty[LEFT], lastDuty[RIGHT], readBusVoltage());

  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type    = HTTPD_WS_TYPE_TEXT;
  pkt.payload = (uint8_t *)json;
  pkt.len     = n;

  esp_err_t ret = httpd_ws_send_frame_async(server, wsClientFd, &pkt);
  if (ret != ESP_OK) {
    wsClientFd = -1;  // client is gone - stop trying; watchdog stops the motors
  }
}

// ---- static file serving (LittleFS) ----
static esp_err_t serveFile(httpd_req_t *req, const char *path, const char *contentType) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, contentType);
  uint8_t buf[512];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    httpd_resp_send_chunk(req, (const char *)buf, n);
  }
  f.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t indexHandler(httpd_req_t *req) { return serveFile(req, "/index.html", "text/html"); }
static esp_err_t cssHandler(httpd_req_t *req)   { return serveFile(req, "/style.css", "text/css"); }
static esp_err_t mixJsHandler(httpd_req_t *req) { return serveFile(req, "/mix.js", "application/javascript"); }
static esp_err_t appJsHandler(httpd_req_t *req) { return serveFile(req, "/app.js", "application/javascript"); }

static bool startWebServer() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return false;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  if (httpd_start(&server, &config) != ESP_OK) {
    Serial.println("httpd_start failed");
    return false;
  }

  httpd_uri_t indexUri = { "/", HTTP_GET, indexHandler, NULL };
  httpd_uri_t cssUri   = { "/style.css", HTTP_GET, cssHandler, NULL };
  httpd_uri_t mixJsUri = { "/mix.js", HTTP_GET, mixJsHandler, NULL };
  httpd_uri_t appJsUri = { "/app.js", HTTP_GET, appJsHandler, NULL };

  httpd_uri_t wsUri;
  memset(&wsUri, 0, sizeof(wsUri));
  wsUri.uri          = "/ws";
  wsUri.method       = HTTP_GET;
  wsUri.handler      = wsHandler;
  wsUri.is_websocket = true;

  httpd_register_uri_handler(server, &indexUri);
  httpd_register_uri_handler(server, &cssUri);
  httpd_register_uri_handler(server, &mixJsUri);
  httpd_register_uri_handler(server, &appJsUri);
  httpd_register_uri_handler(server, &wsUri);

  return true;
}
