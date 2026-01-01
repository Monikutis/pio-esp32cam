#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "esp_camera.h"
#include "motor_pins.h"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

#define PART_BOUNDARY "frame"

static void activateForward() { digitalWrite(PIN_FORWARD, HIGH); digitalWrite(PIN_BACKWARD, LOW); digitalWrite(PIN_LEFT, LOW); digitalWrite(PIN_RIGHT, LOW); }
static void activateBackward() { digitalWrite(PIN_FORWARD, LOW); digitalWrite(PIN_BACKWARD, HIGH); digitalWrite(PIN_LEFT, LOW); digitalWrite(PIN_RIGHT, LOW); }
static void activateLeft() { digitalWrite(PIN_FORWARD, LOW); digitalWrite(PIN_BACKWARD, LOW); digitalWrite(PIN_LEFT, HIGH); digitalWrite(PIN_RIGHT, LOW); }
static void activateRight() { digitalWrite(PIN_FORWARD, LOW); digitalWrite(PIN_BACKWARD, LOW); digitalWrite(PIN_LEFT, LOW); digitalWrite(PIN_RIGHT, HIGH); }
static void activateStop() { digitalWrite(PIN_FORWARD, LOW); digitalWrite(PIN_BACKWARD, LOW); digitalWrite(PIN_LEFT, LOW); digitalWrite(PIN_RIGHT, LOW); }

// WebSocket event handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    String msg = (char*)data;
    msg = msg.substring(0, len);
    if (msg.indexOf("forward") != -1) activateForward();
    else if (msg.indexOf("backward") != -1) activateBackward();
    else if (msg.indexOf("left") != -1) activateLeft();
    else if (msg.indexOf("right") != -1) activateRight();
    else if (msg.indexOf("stop") != -1) activateStop();
  }
}

// Your exact HTML (fixed <img src> typo — it was "srouce" in your message, but correct in file)
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
    <img id="video" src="/stream" alt="Camera Stream" />
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
    let ws = null;
    function connectWS() {
      ws = new WebSocket('ws://' + location.hostname + '/ws');
      ws.onopen = () => console.log("WebSocket connected");
      ws.onclose = () => { console.log("Disconnected - reconnecting..."); setTimeout(connectWS, 1000); };
    }
    connectWS();

    function sendCmd(cmd) {
      console.log("Sending:", cmd);
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({cmd: cmd}));
      } else {
        fetch("/action?cmd=" + cmd);
      }
    }
  </script>
</body>
</html>
)rawliteral";

// MJPEG stream handler
void handleStream(AsyncWebServerRequest *request) {
  AsyncResponseStream *response = request->beginResponseStream("multipart/x-mixed-replace;boundary=" PART_BOUNDARY);
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;
    response->printf("--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", PART_BOUNDARY, fb->len);
    response->write(fb->buf, fb->len);
    response->print("\r\n");
    esp_camera_fb_return(fb);
    delay(1);
  }
}

void startCameraServer() {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/action", HTTP_GET, [](AsyncWebServerRequest *request) {
    String cmd = request->arg("cmd");
    if (cmd == "forward") activateForward();
    else if (cmd == "backward") activateBackward();
    else if (cmd == "left") activateLeft();
    else if (cmd == "right") activateRight();
    else if (cmd == "stop") activateStop();
    request->send(200);
  });

  server.on("/stream", HTTP_GET, handleStream);

  server.begin();
  Serial.println("Async server started");
}