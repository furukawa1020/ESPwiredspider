#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "network.h"

namespace {
constexpr char AP_SSID[] = "WiredSpider";
constexpr char AP_PASSWORD[] = "spider-4826";
AsyncWebServer server(80);
AsyncWebSocket socket("/ws");
QueueHandle_t commands = nullptr;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
DeviceTelemetry state{};
extern const char page[] asm("_binary_web_index_html_start");

void renderState(char* output, size_t size) {
  DeviceTelemetry copy;
  portENTER_CRITICAL(&stateLock);
  copy = state;
  portEXIT_CRITICAL(&stateLock);
  formatState(copy, output, size);
}

void onSocket(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
              void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    auto* request = static_cast<AsyncWebServerRequest*>(arg);
    // Browser control is served by this same device, on this same origin.
    if (request && request->hasHeader("Origin") &&
        request->header("Origin") != "http://192.168.4.1") {
      client->close();
      return;
    }
    char output[1200];
    renderState(output, sizeof(output));
    client->text(output);
    return;
  }
  if (type != WS_EVT_DATA) return;
  auto* frame = static_cast<AwsFrameInfo*>(arg);
  if (!frame->final || frame->index != 0 || frame->len != len ||
      frame->opcode != WS_TEXT || len == 0 || len > 95 || memchr(data, 0, len)) {
    client->text("{\"type\":\"error\",\"message\":\"Invalid frame\"}");
    return;
  }
  char text[96];
  memcpy(text, data, len);
  text[len] = 0;
  ControlCommand command;
  if (!parseControlCommand(text, command)) {
    client->text("{\"type\":\"error\",\"message\":\"Invalid command or range\"}");
    return;
  }
  // Global stop clears pending motion commands and takes precedence.
  if (command.type == CommandType::Stop && command.target == 0) xQueueReset(commands);
  if (xQueueSend(commands, &command, 0) != pdTRUE) {
    client->text("{\"type\":\"error\",\"message\":\"Busy; retry\"}");
    return;
  }
  client->text("{\"type\":\"queued\"}");
}

void telemetryTask(void*) {
  for (;;) {
    socket.cleanupClients();
    if (socket.count() && socket.availableForWriteAll()) {
      char output[1200];
      renderState(output, sizeof(output));
      socket.textAll(output);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
}  // namespace

bool startNetwork() {
  commands = xQueueCreate(16, sizeof(ControlCommand));
  if (!commands) return false;
  WiFi.mode(WIFI_AP);
  const IPAddress ip(192, 168, 4, 1);
  if (!WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(AP_SSID, AP_PASSWORD, 6, false, 4)) return false;
  socket.onEvent(onSocket);
  server.addHandler(&socket);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send_P(200, "text/html; charset=utf-8", page);
  });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* request) {
    char output[1200];
    renderState(output, sizeof(output));
    auto* response = request->beginResponse(200, "application/json", output);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  server.onNotFound([](AsyncWebServerRequest* request) { request->send(404); });
  if (xTaskCreatePinnedToCore(telemetryTask, "telemetry", 8192, nullptr, 1, nullptr, 0) != pdPASS) return false;
  server.begin();
  Serial.println("Wi-Fi AP: WiredSpider / password: spider-4826");
  Serial.println("Dashboard: http://192.168.4.1/  WebSocket: ws://192.168.4.1/ws");
  return true;
}

bool takeNetworkCommand(ControlCommand& command) {
  return commands && xQueueReceive(commands, &command, 0) == pdTRUE;
}

void publishNetworkState(const DeviceTelemetry& snapshot) {
  portENTER_CRITICAL(&stateLock);
  state = snapshot;
  portEXIT_CRITICAL(&stateLock);
}
