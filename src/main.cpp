#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiManager.h>          // Now from khoih-prog fork
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include "motor_pins.h"

// Camera model
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// Globals
AsyncWebServer server(80);          // Control page + OTA
AsyncWebServer streamServer(81);    // MJPEG stream

// Motor functions (unchanged)
void activateForward() {
  digitalWrite(PIN_FORWARD, HIGH); digitalWrite(PIN_BACKWARD, LOW);
  digitalWrite(PIN_LEFT, LOW);     digitalWrite(PIN_RIGHT, LOW);
}
void activateBackward() {
  digitalWrite(PIN_FORWARD, LOW);  digitalWrite(PIN_BACKWARD, HIGH);
  digitalWrite(PIN_LEFT, LOW);     digitalWrite(PIN_RIGHT, LOW);
}
void activateLeft() {
  digitalWrite(PIN_FORWARD, LOW);  digitalWrite(PIN_BACKWARD, LOW);
  digitalWrite(PIN_LEFT, HIGH);    digitalWrite(PIN_RIGHT, LOW);
}
void activateRight() {
  digitalWrite(PIN_FORWARD, LOW);  digitalWrite(PIN_BACKWARD, LOW);
  digitalWrite(PIN_LEFT, LOW);     digitalWrite(PIN_RIGHT, HIGH);
}
void activateStop() {
  digitalWrite(PIN_FORWARD, LOW);  digitalWrite(PIN_BACKWARD, LOW);
  digitalWrite(PIN_LEFT, LOW);     digitalWrite(PIN_RIGHT, LOW);
}

// Command handler
void handleCommand(AsyncWebServerRequest *request) {
  if (request->hasParam("cmd")) {
    String cmd = request->getParam("cmd")->value();
    cmd.trim();
    Serial.printf("[CMD] %s\n", cmd.c_str());

    if      (cmd == "forward")  activateForward();
    else if (cmd == "backward") activateBackward();
    else if (cmd == "left")     activateLeft();
    else if (cmd == "right")    activateRight();
    else if (cmd == "stop")     activateStop();
    else Serial.printf("[CMD] Unknown: %s\n", cmd.c_str());
  }
  request->send(200, "text/plain", "OK");
}

// MJPEG stream handler (async chunked)
void handleStream(AsyncWebServerRequest *request) {
  AsyncResponseStream *response = request->beginChunkedResponse("multipart/x-mixed-replace;boundary=frame", [](
    uint8_t *buffer, size_t maxLen, size_t index) -> size_t {

    static camera_fb_t *fb = nullptr;

    if (index == 0) {
      fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("Capture failed");
        return RESPONSE_TRY_AGAIN;
      }

      size_t hlen = snprintf((char*)buffer, maxLen,
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
        fb->len);

      if (hlen >= maxLen) {
        esp_camera_fb_return(fb); fb = nullptr;
        return RESPONSE_TRY_AGAIN;
      }

      if (hlen + fb->len <= maxLen) {
        memcpy(buffer + hlen, fb->buf, fb->len);
        size_t sent = hlen + fb->len;
        esp_camera_fb_return(fb); fb = nullptr;
        return sent;
      } else {
        esp_camera_fb_return(fb); fb = nullptr;
        return hlen;  // partial send - next call continues
      }
    }

    if (fb) {
      // If we need to continue sending data (rare)
      return 0;
    }

    // Frame end marker for next
    return snprintf((char*)buffer, maxLen, "\r\n--frame\r\n");
  });

  if (response) {
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  } else {
    request->send(500);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("\nESP32 RC Car - WiFiManager + ElegantOTA");

  // Camera init
  camera_config_t config{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_UXGA;
    config.jpeg_quality = 20;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 22;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed 0x%x\n", err);
    delay(3000);
    ESP.restart();
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, FRAMESIZE_QVGA);

  // Motors
  pinMode(PIN_FORWARD, OUTPUT);
  pinMode(PIN_BACKWARD, OUTPUT);
  pinMode(PIN_LEFT, OUTPUT);
  pinMode(PIN_RIGHT, OUTPUT);
  activateStop();

  // WiFiManager (khoih-prog version)
  WiFiManager wm;
  // wm.resetSettings();  // uncomment to reset saved WiFi

  bool res = wm.autoConnect("CamCar-Setup", "password123");  // AP name + optional password
  if (!res) {
    Serial.println("Failed to connect → rebooting");
    delay(3000);
    ESP.restart();
  }

  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  // ElegantOTA
  ElegantOTA.begin(&server);

  // Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
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
    const streamPort = 81;
    document.getElementById('video').src = `http://${window.location.hostname}:${streamPort}/stream`;
    
    let cmdQueue = [];
    let sending = false;
    
    async function sendCmd(cmd) {
      cmdQueue.push(cmd);
      processQueue();
    }
    
    async function processQueue() {
      if (sending || cmdQueue.length === 0) return;
      sending = true;
      const cmd = cmdQueue.shift();
      fetch(`http://${window.location.hostname}/action?cmd=${cmd}`, {method: 'GET', keepalive: true})
        .catch(() => {});
      sending = false;
      if (cmdQueue.length > 0) setTimeout(processQueue, 10);
    }
  </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  server.on("/action", HTTP_GET, handleCommand);
  server.begin();

  // Stream
  streamServer.on("/stream", HTTP_GET, handleStream);
  streamServer.begin();

  Serial.println("Ready!");
}

void loop() {
  ElegantOTA.loop();
  delay(2);
}