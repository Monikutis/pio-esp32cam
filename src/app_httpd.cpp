#include <Arduino.h>
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_camera.h"
#include "motor_pins.h"

static httpd_handle_t server = NULL;
static httpd_handle_t stream_server = NULL;

#define PART_BOUNDARY "frame"
#define _STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY

static void activateForward() { 
  digitalWrite(PIN_FORWARD, HIGH); 
  digitalWrite(PIN_BACKWARD, LOW); 
  digitalWrite(PIN_LEFT, LOW); 
  digitalWrite(PIN_RIGHT, LOW); 
}

static void activateBackward() { 
  digitalWrite(PIN_FORWARD, LOW); 
  digitalWrite(PIN_BACKWARD, HIGH); 
  digitalWrite(PIN_LEFT, LOW); 
  digitalWrite(PIN_RIGHT, LOW); 
}

static void activateLeft() { 
  digitalWrite(PIN_FORWARD, LOW); 
  digitalWrite(PIN_BACKWARD, LOW); 
  digitalWrite(PIN_LEFT, HIGH); 
  digitalWrite(PIN_RIGHT, LOW); 
}

static void activateRight() { 
  digitalWrite(PIN_FORWARD, LOW); 
  digitalWrite(PIN_BACKWARD, LOW); 
  digitalWrite(PIN_LEFT, LOW); 
  digitalWrite(PIN_RIGHT, HIGH); 
}

static void activateStop() { 
  digitalWrite(PIN_FORWARD, LOW); 
  digitalWrite(PIN_BACKWARD, LOW); 
  digitalWrite(PIN_LEFT, LOW); 
  digitalWrite(PIN_RIGHT, LOW); 
}

static esp_err_t open_handler(httpd_handle_t hd, int sockfd) {
    Serial.printf("[SERVER] Client connected. Socket: %d\n", sockfd);
    return ESP_OK;
}

static void close_handler(httpd_handle_t hd, int sockfd) {
    Serial.printf("[SERVER] Client disconnected. Socket: %d\n", sockfd);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[128];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!jpeg_converted) {
        Serial.println("JPEG compression failed");
        res = ESP_FAIL;
        break;
      }
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    size_t hlen = snprintf((char *)part_buf, 128,
      "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      _jpg_buf_len);

    res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, "\r\n--" PART_BOUNDARY "\r\n", -1);
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }

    if (res != ESP_OK) break;

    yield();
    vTaskDelay(2 / portTICK_PERIOD_MS); 
  }

  httpd_resp_send_chunk(req, NULL, 0); 
  return res;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char* buf;
  size_t buf_len;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      char param[32];
      if (httpd_query_key_value(buf, "cmd", param, sizeof(param)) == ESP_OK) {
        String cmd = String(param);
        cmd.trim();
        Serial.printf("[CMD] Parsed cmd: '%s'\n", cmd.c_str());

        if (cmd == "forward")      { activateForward();     Serial.println("[CMD] Forward activated"); }
        else if (cmd == "backward") { activateBackward();   Serial.println("[CMD] Backward activated"); }
        else if (cmd == "left")     { activateLeft();       Serial.println("[CMD] Left activated"); }
        else if (cmd == "right")    { activateRight();      Serial.println("[CMD] Right activated"); }
        else if (cmd == "stop")     { activateStop();       Serial.println("[CMD] Stop activated"); }
        else {
          Serial.printf("[CMD] Unknown command: '%s'\n", cmd.c_str());
        }
      } else {
        Serial.println("[CMD] Failed to parse 'cmd' parameter");
      }
    } else {
      Serial.println("[CMD] Failed to get query string");
    }

    free(buf);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
  const char* index_html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>RC Car Control</title>
    <style>
      * { margin: 0; padding: 0; box-sizing: border-box; }
      body { font-family: Arial, sans-serif; background: #f0f0f0; height: 100vh; display: flex; flex-direction: column; overflow: hidden; }
      #video-container { flex: 1; position: relative; background: black; overflow: hidden; }
      #video { width: 100%; height: 100%; object-fit: cover; }
      .controls-overlay { position: absolute; bottom: 20px; left: 0; right: 0; display: flex; justify-content: space-between; align-items: flex-end; padding: 0 30px; pointer-events: none; }
      .control-group { display: flex; flex-direction: column; gap: 30px; pointer-events: auto; }
      .control-group.right { align-items: flex-end; }
      button { width: 90px; height: 90px; background: #007bff; border: none; border-radius: 20px; font-size: 50px; color: white; box-shadow: 0 10px 0 #0056b3, 0 15px 25px rgba(0,0,0,0.4); cursor: pointer; transition: all 0.1s; user-select: none; }
      button:active { transform: translateY(6px); box-shadow: 0 4px 0 #0056b3, 0 8px 15px rgba(0,0,0,0.4); }
    </style>
</head>
<body>
  <div id="video-container">
    <img id="video" alt="Camera Stream" />
    <div class="controls-overlay">
      <div class="control-group">
        <button onmousedown="sendCmd('forward')" ontouchstart="sendCmd('forward')" onmouseup="sendCmd('stop')" ontouchend="sendCmd('stop')">↑</button>
        <button onmousedown="sendCmd('backward')" ontouchstart="sendCmd('backward')" onmouseup="sendCmd('stop')" ontouchend="sendCmd('stop')">↓</button>
      </div>
      <div class="control-group right">
        <button onmousedown="sendCmd('right')" ontouchstart="sendCmd('right')" onmouseup="sendCmd('stop')" ontouchend="sendCmd('stop')">→</button>
        <button onmousedown="sendCmd('left')" ontouchstart="sendCmd('left')" onmouseup="sendCmd('stop')" ontouchend="sendCmd('stop')">←</button>
      </div>
    </div>
  </div>

  <script>
    // Use different port for stream to avoid blocking
    const streamPort = 81;
    const cmdPort = 80;
    
    // Set stream source
    document.getElementById('video').src = `http://${window.location.hostname}:${streamPort}/stream`;
    
    // Keep-alive connection pool for commands
    let cmdQueue = [];
    let sending = false;
    
    async function sendCmd(cmd) {
      console.log("Sending:", cmd);
      
      // Add to queue
      cmdQueue.push(cmd);
      processQueue();
    }
    
    async function processQueue() {
      if (sending || cmdQueue.length === 0) return;
      
      sending = true;
      const cmd = cmdQueue.shift();
      
      try {
        // Fire and forget 
        fetch(`http://${window.location.hostname}:${cmdPort}/action?cmd=${cmd}`, {
          method: 'GET',
          keepalive: true
        }).catch(() => {});
        
      } catch {
        //
      }
      
      sending = false;
      
      // Process next command
      if (cmdQueue.length > 0) {
        setTimeout(processQueue, 10);
      }
    }
  </script>
</body>
</html>
)rawliteral";

  httpd_resp_send(req, index_html, strlen(index_html));
  return ESP_OK;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 16;
  config.open_fn = open_handler;
  config.close_fn = close_handler;

  Serial.printf("Starting command server on port: '%d'\n", config.server_port);

  if (httpd_start(&server, &config) == ESP_OK) {
    Serial.println("Command HTTP server started successfully");

    // Root page
    static const httpd_uri_t index_uri = {
      .uri       = "/",
      .method    = HTTP_GET,
      .handler   = index_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &index_uri);

    // Command
    static const httpd_uri_t cmd_uri = {
      .uri       = "/action",
      .method    = HTTP_GET,
      .handler   = cmd_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &cmd_uri);
  } else {
    Serial.println("Error starting command server!");
  }

  // Server 2: Video Stream (Port 81)
  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;
  stream_config.max_open_sockets = 3;
  stream_config.lru_purge_enable = true;
  stream_config.max_uri_handlers = 4;

  Serial.printf("Starting stream server on port: '%d'\n", stream_config.server_port);

  if (httpd_start(&stream_server, &stream_config) == ESP_OK) {
    Serial.println("Stream HTTP server started successfully");

    // Stream
    static const httpd_uri_t stream_uri = {
      .uri       = "/stream",
      .method    = HTTP_GET,
      .handler   = stream_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(stream_server, &stream_uri);
  } else {
    Serial.println("Error starting stream server!");
  }
}