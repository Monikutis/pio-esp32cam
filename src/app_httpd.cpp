#include <Arduino.h>
#include "esp_http_server.h"
#include "esp_camera.h"

extern AsyncWebServer streamServer;   // declared in main.cpp

#define PART_BOUNDARY "frame"
#define _STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY

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

void startCameraServer() {
  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;
  stream_config.max_open_sockets = 3;
  stream_config.lru_purge_enable = true;
  stream_config.max_uri_handlers = 4;

  httpd_handle_t &stream_server = *(httpd_handle_t*)streamServer.getServer(); // little trick if needed — but we use global

  Serial.printf("Starting stream server on port %d\n", stream_config.server_port);

  if (httpd_start(&stream_server, &stream_config) == ESP_OK) {
    Serial.println("Stream HTTP server started");

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