#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <sys/time.h>
#include "controller.h"

namespace {
constexpr uint8_t PINS[3][3] = {{13,14,16}, {18,19,21}, {25,26,27}};
constexpr uint8_t STANDBY = 23;
constexpr uint8_t DUTY = 128; // 50% for initial DC motor commissioning.
QueueHandle_t commandQueue, eventQueue, snapshotQueue;
portMUX_TYPE linkLock = portMUX_INITIALIZER_UNLOCKED;
bool ready = false;
uint32_t lastPong = 0;
uint8_t activeOutputs = 0;
struct Event { char id[64]; char status[16]; char message[80]; };
struct Snapshot { bool active[3]; bool pending[3]; int8_t direction[3]; uint32_t remainingMs[3]; };
struct Config { String ssid, password, host, path, token, deviceId, ca; uint16_t port = 443; };
Config cfg;
WebSocketsClient ws;
bool configured = false, socketStarted = false, wifiStarted = false;
String authHeader;

uint64_t utcMs() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}
void setLink(bool value) {
  portENTER_CRITICAL(&linkLock);
  ready = value;
  if (value) lastPong = millis();
  portEXIT_CRITICAL(&linkLock);
}
bool linkHealthy() {
  portENTER_CRITICAL(&linkLock);
  const bool value = ready && static_cast<uint32_t>(millis() - lastPong) < 2000;
  portEXIT_CRITICAL(&linkLock);
  return value;
}
void report(const char* id, const char* status, const char* message) {
  Event event{};
  strlcpy(event.id, id, sizeof(event.id));
  strlcpy(event.status, status, sizeof(event.status));
  strlcpy(event.message, message, sizeof(event.message));
  xQueueSend(eventQueue, &event, 0);
}
void release(uint8_t axis) {
  ledcWrite(axis, 0);
  digitalWrite(PINS[axis][0], HIGH);
  digitalWrite(PINS[axis][1], HIGH);
  activeOutputs &= ~(1u << axis);
  if (!activeOutputs) {
    digitalWrite(STANDBY, LOW);
    for (const auto& pins : PINS) {
      digitalWrite(pins[0], LOW);
      digitalWrite(pins[1], LOW);
    }
  }
}
void drive(uint8_t axis, int8_t direction) {
  ledcWrite(axis, 0);
  digitalWrite(PINS[axis][0], direction > 0 ? HIGH : LOW);
  digitalWrite(PINS[axis][1], direction > 0 ? LOW : HIGH);
  activeOutputs |= 1u << axis;
  digitalWrite(STANDBY, HIGH);
  ledcWrite(axis, DUTY);
}
rail::Controller controller({drive, release, report});

bool plain(const String& value) { return value.indexOf('\r') < 0 && value.indexOf('\n') < 0; }
bool loadConfig(JsonVariantConst root, Config& out) {
  if (!root["wifi_ssid"].is<const char*>() || !root["wifi_password"].is<const char*>() ||
      !root["server_host"].is<const char*>() || !root["device_token"].is<const char*>() ||
      !root["root_ca"].is<const char*>()) return false;
  out.ssid = root["wifi_ssid"].as<String>();
  out.password = root["wifi_password"].as<String>();
  out.host = root["server_host"].as<String>();
  out.token = root["device_token"].as<String>();
  out.ca = root["root_ca"].as<String>();
  out.path = root["server_path"] | "/ws/device";
  out.deviceId = root["device_id"] | "rail-1";
  const int port = root["server_port"] | 443;
  if (out.ssid.length() < 1 || out.ssid.length() > 32 || out.password.length() > 63 ||
      out.host.length() < 1 || out.host.length() > 253 || out.host.indexOf('/') >= 0 ||
      out.host.indexOf(' ') >= 0 || !plain(out.host) || port < 1 || port > 65535 ||
      !out.path.startsWith("/") || out.path.length() > 128 || !plain(out.path) ||
      out.token.length() < 24 || out.token.length() > 256 || !plain(out.token) ||
      out.deviceId.length() < 1 || out.deviceId.length() > 64 ||
      out.ca.indexOf("-----BEGIN CERTIFICATE-----") < 0 ||
      out.ca.indexOf("-----END CERTIFICATE-----") < 0 || out.ca.length() > 6000) return false;
  out.port = port;
  return true;
}

void printStatus() {
  Snapshot snapshot{};
  xQueuePeek(snapshotQueue, &snapshot, 0);
  StaticJsonDocument<768> doc;
  doc["type"] = "status";
  doc["firmware"] = "rail-dc-xyz-1.0";
  doc["configured"] = configured;
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["server_connected"] = linkHealthy();
  doc["pwm_duty"] = DUTY;
  auto axes = doc.createNestedArray("axes");
  for (int i = 0; i < 3; ++i) {
    auto a = axes.createNestedObject();
    a["axis"] = i == 0 ? "X" : i == 1 ? "Y" : "Z";
    a["active"] = snapshot.active[i];
    a["pending"] = snapshot.pending[i];
    a["remaining_ms"] = snapshot.remainingMs[i];
  }
  serializeJson(doc, Serial);
  Serial.println();
}

void processSerialLine(const String& text) {
  if (text == "?" || text == "status") { printStatus(); return; }
  if (text == "s" || text == "stop") {
    setLink(false);
    xQueueReset(commandQueue);
    ws.disconnect();
    Serial.println("STOP requested; all DC outputs disabled by control task");
    return;
  }
  DynamicJsonDocument doc(10000);
  if (deserializeJson(doc, text) || doc["op"] != "configure") {
    Serial.println("Use status, stop, or a configure JSON line.");
    return;
  }
  Config candidate;
  if (!loadConfig(doc.as<JsonVariantConst>(), candidate)) {
    Serial.println("Configuration rejected: required Wi-Fi, host, token or CA fields are invalid.");
    return;
  }
  setLink(false);
  xQueueReset(commandQueue);
  vTaskDelay(pdMS_TO_TICKS(20));
  Preferences preferences;
  if (!preferences.begin("rail-dc", false)) { Serial.println("Configuration save failed"); return; }
  doc.remove("op");
  String saved;
  serializeJson(doc, saved);
  const size_t written = preferences.putString("config", saved);
  preferences.end();
  if (written == 0) { Serial.println("Configuration save failed"); return; }
  Serial.println("Configuration saved; restarting stopped.");
  Serial.flush();
  ESP.restart();
}

void onWebSocket(WStype_t type, uint8_t* data, size_t length) {
  if (type == WStype_DISCONNECTED) {
    setLink(false);
    xQueueReset(commandQueue);
    return;
  }
  if (type == WStype_CONNECTED) {
    setLink(false);
    StaticJsonDocument<256> hello;
    hello["type"] = "hello";
    hello["device_id"] = cfg.deviceId;
    hello["simulated"] = false;
    auto axes = hello.createNestedArray("axes");
    axes.add("x"); axes.add("y"); axes.add("z");
    String payload;
    serializeJson(hello, payload);
    ws.sendTXT(payload);
    return;
  }
  if (type != WStype_TEXT || length > 4096) return;
  StaticJsonDocument<1536> doc;
  if (deserializeJson(doc, data, length)) return;
  const char* kind = doc["type"] | "";
  if (!strcmp(kind, "welcome") || !strcmp(kind, "heartbeat")) {
    if (doc["server_time_ms"].is<uint64_t>()) {
      const uint64_t serverTime = doc["server_time_ms"];
      const uint64_t localTime = utcMs();
      const uint64_t skew = serverTime > localTime ? serverTime - localTime : localTime - serverTime;
      if (localTime > 1700000000000ULL && skew < 5000) setLink(true);
    }
    return;
  }
  const bool stopping = !strcmp(kind, "stop");
  if (!stopping && strcmp(kind, "move")) return;
  rail::Command cmd;
  const char* id = doc["command_id"] | "";
  if (!id[0] || strlen(id) >= sizeof(cmd.id)) return;
  strcpy(cmd.id, id);
  cmd.stop = stopping;
  const char* axis = doc["axis"] | "";
  cmd.axis = (!strcmp(axis,"x") || !strcmp(axis,"X")) ? 0 :
             (!strcmp(axis,"y") || !strcmp(axis,"Y")) ? 1 :
             (!strcmp(axis,"z") || !strcmp(axis,"Z")) ? 2 : -1;
  if (cmd.axis == -1 && (!stopping || !doc["axis"].isNull())) {
    report(id, "failed", "Invalid axis"); return;
  }
  if (!stopping) {
    if (!linkHealthy() || !doc["direction"].is<int>() || !doc["duration_ms"].is<uint32_t>() ||
        !doc["expires_at_ms"].is<uint64_t>()) { report(id, "failed", "Invalid move or connection not ready"); return; }
    const int direction = doc["direction"];
    if (direction != 1 && direction != -1) { report(id, "failed", "Invalid direction"); return; }
    cmd.direction = direction;
    cmd.durationMs = doc["duration_ms"];
    cmd.expiresAtMs = doc["expires_at_ms"];
  }
  if (stopping && cmd.axis == -1) xQueueReset(commandQueue);
  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) report(id, "failed", "Command queue full");
}

void networkTask(void*) {
  Preferences preferences;
  if (preferences.begin("rail-dc", true)) {
    String saved = preferences.getString("config", "");
    preferences.end();
    if (saved.length()) {
      DynamicJsonDocument doc(10000);
      if (!deserializeJson(doc, saved)) configured = loadConfig(doc.as<JsonVariantConst>(), cfg);
    }
  }
  Serial.println("RAIL DC XYZ v1.0 ready. Outputs stopped; STBY LOW.");
  printStatus();
  ws.onEvent(onWebSocket);
  ws.setReconnectInterval(3000);
  String serialLine;
  serialLine.reserve(8192);
  bool overflow = false;
  uint32_t lastHeartbeat = 0;
  for (;;) {
    for (unsigned i = 0; i < 128 && Serial.available(); ++i) {
      const char c = Serial.read();
      if (c == '?' && serialLine.isEmpty()) { printStatus(); continue; }
      if (c == '\n') {
        if (!overflow && serialLine.length()) processSerialLine(serialLine);
        else if (overflow) Serial.println("Configuration line too long");
        serialLine = ""; overflow = false;
      } else if (c != '\r' && !overflow) {
        if (serialLine.length() >= 8192) overflow = true;
        else serialLine += c;
      }
    }
    if (configured && !wifiStarted) {
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      wifiStarted = true;
    }
    if (wifiStarted && WiFi.status() == WL_CONNECTED && utcMs() > 1700000000000ULL && !socketStarted) {
      authHeader = "Authorization: Bearer " + cfg.token + "\r\n";
      ws.beginSslWithCA(cfg.host.c_str(), cfg.port, cfg.path.c_str(), cfg.ca.c_str());
      ws.setExtraHeaders(authHeader.c_str());
      socketStarted = true;
    }
    if (socketStarted) {
      ws.loop();
      if (ws.isConnected() && static_cast<uint32_t>(millis() - lastHeartbeat) >= 1000) {
        lastHeartbeat = millis();
        ws.sendTXT("{\"type\":\"heartbeat\"}");
      }
    }
    Event event;
    for (unsigned i = 0; i < 4 && xQueueReceive(eventQueue, &event, 0) == pdTRUE; ++i) {
      if (!ws.isConnected()) continue;
      StaticJsonDocument<384> doc;
      doc["type"] = "event";
      doc["command_id"] = event.id;
      doc["status"] = event.status;
      doc["message"] = event.message;
      String payload;
      serializeJson(doc, payload);
      ws.sendTXT(payload);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}  // namespace

void setup() {
  digitalWrite(STANDBY, LOW);
  pinMode(STANDBY, OUTPUT);
  // Also de-energize the two pins used only by the previous ULN2003 wiring.
  for (uint8_t pin : {17,22}) { digitalWrite(pin, LOW); pinMode(pin, OUTPUT); }
  for (uint8_t i = 0; i < 3; ++i) {
    for (uint8_t pin : PINS[i]) { digitalWrite(pin, LOW); pinMode(pin, OUTPUT); }
    ledcSetup(i, 20000, 8);
    ledcAttachPin(PINS[i][2], i);
    ledcWrite(i, 0);
  }
  Serial.begin(115200);
  commandQueue = xQueueCreate(16, sizeof(rail::Command));
  eventQueue = xQueueCreate(64, sizeof(Event));
  snapshotQueue = xQueueCreate(1, sizeof(Snapshot));
  if (!commandQueue || !eventQueue || !snapshotQueue ||
      xTaskCreatePinnedToCore(networkTask, "rail-network", 20000, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("Startup failed; outputs remain stopped");
    for (;;) delay(1000);
  }
}

void loop() {
  if (!linkHealthy()) controller.stopAll("Connection lost or heartbeat timeout");
  if (uxQueueSpacesAvailable(eventQueue) < 8) {
    controller.stopAll("Event queue congested");
    xQueueReset(commandQueue);
  }
  rail::Command command;
  for (unsigned i = 0; i < 4 && xQueueReceive(commandQueue, &command, 0) == pdTRUE; ++i) {
    if (command.stop || linkHealthy()) controller.apply(command, millis(), utcMs());
    else report(command.id, "failed", "Connection not ready");
  }
  const uint32_t now = millis();
  controller.tick(now, utcMs());
  Snapshot snapshot{};
  for (unsigned i = 0; i < 3; ++i) {
    const auto& axis = controller.axis(i);
    snapshot.active[i] = axis.active;
    snapshot.pending[i] = axis.pending;
    snapshot.direction[i] = axis.direction;
    const uint32_t elapsed = now - axis.since;
    snapshot.remainingMs[i] = axis.active && elapsed < axis.durationMs ? axis.durationMs - elapsed : 0;
  }
  xQueueOverwrite(snapshotQueue, &snapshot);
  delay(1);
}
