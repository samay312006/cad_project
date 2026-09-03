#pragma once

// ws_handler.h - esp_http_server: static file serving from LittleFS, the
// /ws WebSocket endpoint (drive commands in, telemetry out), and the
// 400ms command watchdog. Depends on drive_core.h being included first.

#include <esp_http_server.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

  char *lend = nullptr;
  char *rend = nullptr;
  long l = strtol(lp + 1, &lend, 10);
  long r = strtol(rp + 1, &rend, 10);
  // strtol sets endptr == start when no digits were consumed at all (e.g.
  // a string value like "oops") - reject those instead of silently
  // coercing to 0.
  if (lend == lp + 1 || rend == rp + 1) return false;
  if (l < -255 || l > 255 || r < -255 || r > 255) return false;

  *left  = (int)l;
  *right = (int)r;
  return true;
}

// Best-effort attempt to drain a WS frame's remaining payload off the
// socket when we couldn't allocate a buffer to actually receive it into
// (malloc() failure under heap pressure). Reuses the small fixed-size
// scratch buffer that's always available on the stack/static storage,
// discarding whatever it reads.
//
// This is deliberately "best effort," not a guarantee: whether repeated
// httpd_ws_recv_frame() calls on the same frame correctly walk forward
// through the remaining bytes (vs. re-reading/misaligning) is an ESP-IDF
// internal behavior we can't verify without hardware/toolchain access here.
// The goal is only to reduce the odds of leaving unread bytes on the wire
// that desync the next frame read - not to guarantee it. Bounded by a small
// iteration cap so a bogus/huge length field can never turn this into an
// unbounded loop; after the cap we give up and return anyway.
static void wsDrainBestEffort(httpd_req_t *req, httpd_ws_frame_t *pkt,
                               uint8_t *scratch, size_t scratchCap) {
  static const int kMaxDrainIterations = 8;  // ~4KB at 512B/iter - comfortably covers the 1024B cap
  for (int i = 0; i < kMaxDrainIterations; i++) {
    pkt->payload = scratch;
    esp_err_t r = httpd_ws_recv_frame(req, pkt, scratchCap);
    if (r != ESP_OK) break;  // socket already errored/closed - nothing more to drain
  }
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

  // First call with a NULL/zero-length buffer just reports pkt.len and
  // pkt.type from the frame header, without consuming the payload.
  esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
  if (ret != ESP_OK) return ret;

  // A close frame may legally carry zero bytes of payload (RFC6455), so
  // this must be checked before the len==0 early return below - otherwise
  // a zero-payload close never reaches this branch and the client/motor
  // state is never cleaned up.
  if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
    Serial.println("WS client disconnected");
    wsClientFd = -1;
    stopAll();
    return ESP_OK;
  }

  size_t frameLen = pkt.len;
  if (frameLen == 0) return ESP_OK;

  // 512 bytes comfortably covers any real drive-command message from this
  // sketch's own frontend (matches the buffer size already used by the
  // file-serving code below). Anything bigger goes through the malloc'd
  // path so the socket is always fully drained instead of desyncing.
  static uint8_t buf[512];
  static const size_t MAX_WS_MSG = 1024;  // sane cap against a bogus length field

  uint8_t *readBuf = buf;
  uint8_t *heapBuf = nullptr;
  size_t copyLen = frameLen;   // bytes to actually request in the second recv call
  bool discard = false;

  if (frameLen >= sizeof(buf)) {
    if (frameLen <= MAX_WS_MSG) {
      // Genuinely oversized but sane: allocate exactly enough to read the
      // whole frame in one call so the transport is fully drained.
      heapBuf = (uint8_t *)malloc(frameLen + 1);
      if (!heapBuf) {
        // Can't allocate a buffer to receive into - fall back to a
        // best-effort drain with the small static buffer instead of
        // leaving the frame's bytes sitting unread on the socket (see
        // wsDrainBestEffort() below for why this is best-effort only).
        wsDrainBestEffort(req, &pkt, buf, sizeof(buf));
        return ESP_ERR_NO_MEM;
      }
      readBuf = heapBuf;
      copyLen = frameLen;
    } else {
      // Bogus/huge length field - best-effort drain up to the cap so the
      // transport-level read completes, then discard without parsing.
      Serial.printf("WS frame too large (%u bytes), draining and discarding\n",
                     (unsigned)frameLen);
      heapBuf = (uint8_t *)malloc(MAX_WS_MSG);
      if (!heapBuf) {
        // Same fallback as above.
        wsDrainBestEffort(req, &pkt, buf, sizeof(buf));
        return ESP_ERR_NO_MEM;
      }
      readBuf = heapBuf;
      copyLen = MAX_WS_MSG;
      discard = true;
    }
  }

  pkt.payload = readBuf;
  ret = httpd_ws_recv_frame(req, &pkt, copyLen);
  if (ret != ESP_OK) {
    if (heapBuf) free(heapBuf);
    return ret;
  }

  if (discard || pkt.type != HTTPD_WS_TYPE_TEXT) {
    if (heapBuf) free(heapBuf);
    return ESP_OK;
  }

  size_t n = copyLen;
  if (!heapBuf && n > sizeof(buf) - 1) n = sizeof(buf) - 1;
  readBuf[n] = '\0';

  int left, right;
  if (parseDriveMessage((const char *)readBuf, &left, &right)) {
    drive(left, right);
    lastDriveMsgMs = millis();
  }

  if (heapBuf) free(heapBuf);
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

  // readCurrents() sets .amps to NAN when a sensor is absent. %.3f on NaN
  // formats as "nan"/"-nan", which is not a valid JSON token and would
  // break JSON.parse() on the GUI side for the life of the connection -
  // substitute a safe numeric value instead.
  float leftAmps  = isnan(isense[LEFT].amps)  ? 0.0f : isense[LEFT].amps;
  float rightAmps = isnan(isense[RIGHT].amps) ? 0.0f : isense[RIGHT].amps;

  char json[192];
  int n = snprintf(json, sizeof(json),
    "{\"type\":\"telemetry\",\"left_a\":%.3f,\"right_a\":%.3f,"
    "\"left_duty\":%d,\"right_duty\":%d,\"vbus\":%.2f}",
    leftAmps, rightAmps,
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
