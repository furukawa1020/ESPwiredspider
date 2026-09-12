#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <WebServer.h>
#include <sys/time.h>
#include <driver/rmt.h>
#include <esp_wifi.h>
#include <atomic>
#include "controller.h"

namespace {
constexpr uint8_t MOTOR_IN1 = 18;
constexpr uint8_t MOTOR_IN2 = 19;
constexpr uint8_t RGB_PIN = 16;
bool rgbReady = false;
uint32_t lastRgb = UINT32_MAX;
constexpr uint8_t STANDBY = 23;
QueueHandle_t commandQueue, eventQueue, snapshotQueue;
portMUX_TYPE linkLock = portMUX_INITIALIZER_UNLOCKED;
bool ready = false;
uint32_t lastPong = 0;
struct Event { char id[64]; char status[16]; char message[80]; };
struct Snapshot { bool active[3]; bool pending[3]; int8_t direction[3]; uint32_t remainingMs[3]; uint8_t rgb[3]; };
struct Config { String ssid, password, host, path, token, deviceId, ca; uint16_t port = 443; };
Config cfg;
WebSocketsClient ws;
bool configured = false, socketStarted = false, wifiStarted = false;
String authHeader;
WebServer http(80);
String apName, apPassword;
std::atomic<uint32_t> apStarts{0}, apStops{0}, httpRequests{0};

const char CONTROL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>レール操作</title><style>body{font:18px system-ui;max-width:640px;margin:32px auto;padding:16px}button,input{font:inherit;padding:12px;margin:6px}pre{white-space:pre-wrap;font-size:14px}#stop{background:#ad1717;color:white}</style>
<h1>レール操作</h1><p>X軸・GPIO18 / GPIO19</p>
<label>動作時間（ミリ秒）<input id="duration" type="number" min="1" max="60000" step="1" value="500"></label>
<p><button onclick="move(1)">正転</button><button onclick="move(-1)">逆転</button><button id="stop" onclick="send('/stop',{axis:null})">停止</button></p>
<p id="result" role="status">接続確認中</p><pre id="state"></pre>
<script>
async function send(path,body){try{const r=await fetch('/api/v1/rail'+path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const data=await r.json();document.querySelector('#result').textContent=r.ok?'指令を受け付けました':JSON.stringify(data);await fetchStatus();}catch(e){document.querySelector('#result').textContent='通信できません。Wi-Fi接続を確認してください。'}}
function move(direction){const duration_ms=Number(document.querySelector('#duration').value);if(!Number.isInteger(duration_ms)||duration_ms<1||duration_ms>60000){document.querySelector('#result').textContent='1〜60000ミリ秒の整数を入力してください。';return;}send('/move',{axis:'x',direction,duration_ms});}
async function fetchStatus(){try{const r=await fetch('/api/v1/rail/status',{cache:'no-store'});if(!r.ok)throw Error(r.status);document.querySelector('#state').textContent=JSON.stringify(await r.json(),null,2);if(document.querySelector('#result').textContent==='接続確認中')document.querySelector('#result').textContent='接続できています';}catch(e){document.querySelector('#state').textContent='状態を取得できません';}}
async function poll(){await fetchStatus();setTimeout(poll,2000);}poll();
</script></html>)HTML";

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
  if (axis != 0) return;
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
}
void drive(uint8_t axis, int8_t direction) {
  if (axis != 0) return;
  release(axis);
  digitalWrite(direction > 0 ? MOTOR_IN1 : MOTOR_IN2, HIGH);
}
rail::Controller controller({drive, release, report});

void showRgb(uint8_t r, uint8_t g, uint8_t b) {
  const uint32_t grb = (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(r) << 8) | b;
  if (!rgbReady || grb == lastRgb) return;
  // APB 80 MHz / 2 = 25 ns per tick. WS2812 GRB, 1.25 us per bit.
  rmt_item32_t bits[24] = {};
  for (unsigned i = 0; i < 24; ++i) {
    const bool one = grb & (1u << (23 - i));
    bits[i].level0 = 1; bits[i].duration0 = one ? 28 : 14;
    bits[i].level1 = 0; bits[i].duration1 = one ? 22 : 36;
  }
  bits[23].duration1 += 12000; // 300 us reset/latch interval.
  if (rmt_write_items(RMT_CHANNEL_0, bits, 24, true) == ESP_OK) lastRgb = grb;
}

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

String statusJson() {
  Snapshot snapshot{};
  xQueuePeek(snapshotQueue, &snapshot, 0);
  StaticJsonDocument<2048> doc;
  doc["type"] = "status";
  doc["firmware"] = "rail-dc-xyz-1.0";
  doc["network_revision"] = 2;
  doc["uptime_ms"] = millis();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["ap_starts"] = apStarts.load();
  doc["ap_stops"] = apStops.load();
  doc["http_requests"] = httpRequests.load();
  doc["ap_clients"] = WiFi.softAPgetStationNum();
  doc["wifi_mode"] = WiFi.getMode() == WIFI_AP ? "ap" : "ap_sta";
  uint8_t channel = 0;
  wifi_second_chan_t secondary;
  if (esp_wifi_get_channel(&channel, &secondary) == ESP_OK) doc["ap_channel"] = channel;
  doc["mode"] = "dc_l9110s";
  doc["simulated"] = false;
  doc["driver"] = "L9110S";
  doc["gpio18"] = digitalRead(MOTOR_IN1);
  doc["gpio19"] = digitalRead(MOTOR_IN2);
  doc["rgb_led_ready"] = rgbReady;
  auto rgb = doc.createNestedArray("led_rgb");
  for (uint8_t value : snapshot.rgb) rgb.add(value);
  doc["configured"] = configured;
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["server_connected"] = linkHealthy();
  doc["drive_mode"] = "high_low";
  doc["standby_pin_high"] = digitalRead(STANDBY) == HIGH;
  doc["http_port"] = 80;
  doc["http_auth_required"] = false;
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["ap_ssid"] = apName;
  doc["ip"] = configured ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  auto axes = doc.createNestedArray("axes");
  for (int i = 0; i < 3; ++i) {
    auto a = axes.createNestedObject();
    a["axis"] = i == 0 ? "x" : i == 1 ? "y" : "z";
    a["available"] = i == 0;
    a["active"] = snapshot.active[i];
    a["pending"] = snapshot.pending[i];
    a["direction"] = snapshot.direction[i];
    a["remaining_ms"] = snapshot.remainingMs[i];
  }
  String result;
  serializeJson(doc, result);
  return result;
}
void printStatus() { Serial.println(statusJson()); }

void localStop() {
  rail::Command cmd;
  cmd.local = true; cmd.stop = true;
  snprintf(cmd.id, sizeof(cmd.id), "serial-%08x-%08x", esp_random(), millis());
  xQueueReset(commandQueue);
  xQueueSend(commandQueue, &cmd, 0);
}

void httpCommand(bool stopping) {
  ++httpRequests;
  StaticJsonDocument<512> doc;
  if (!http.header("Content-Type").startsWith("application/json") ||
      http.arg("plain").length() > 512 || deserializeJson(doc, http.arg("plain")) ||
      !doc.is<JsonObject>()) {
    http.send(400, "application/json", "{\"error\":\"expected JSON object\"}"); return;
  }
  rail::Command cmd;
  cmd.local = true; cmd.stop = stopping;
  const char* axis = doc["axis"] | "";
  cmd.axis = !strcmp(axis,"x") ? 0 : !strcmp(axis,"y") ? 1 : !strcmp(axis,"z") ? 2 : -1;
  if (!stopping && cmd.axis != 0) {
    http.send(422, "application/json", "{\"error\":\"Only axis x has a connected L9110S motor\"}"); return;
  }
  if ((cmd.axis < 0 && (!stopping || !doc["axis"].isNull())) ||
      (!stopping && (!doc["direction"].is<int>() || !doc["duration_ms"].is<uint32_t>() ||
        (doc["direction"].as<int>() != 1 && doc["direction"].as<int>() != -1) ||
        doc["duration_ms"].as<uint32_t>() < 1 || doc["duration_ms"].as<uint32_t>() > 60000))) {
    http.send(422, "application/json", "{\"error\":\"invalid axis, direction or duration_ms\"}"); return;
  }
  cmd.direction = doc["direction"] | 0;
  cmd.durationMs = doc["duration_ms"] | 0u;
  // Local requests need no NTP; only the bounded queue residence is limited here.
  cmd.expiresAtMs = utcMs() + 2000;
  snprintf(cmd.id, sizeof(cmd.id), "http-%08x-%08x", esp_random(), millis());
  if (stopping && cmd.axis == -1) xQueueReset(commandQueue);
  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    http.send(503, "application/json", "{\"error\":\"command queue full\"}"); return;
  }
  StaticJsonDocument<192> response;
  response["command_id"] = cmd.id;
  response["status"] = "accepted";
  String body; serializeJson(response, body);
  http.send(202, "application/json", body);
}

void httpTask(void*) {
  http.enableCORS(true);
  const char* headers[] = {"Content-Type"};
  http.collectHeaders(headers, 1);
  http.on("/api/v1/rail/move", HTTP_POST, [] { httpCommand(false); });
  http.on("/api/v1/rail/stop", HTTP_POST, [] { httpCommand(true); });
  http.on("/api/v1/rail/status", HTTP_GET, [] {
    ++httpRequests;
    http.send(200, "application/json", statusJson());
  });
  http.on("/", HTTP_GET, [] { ++httpRequests; http.send_P(200, "text/html; charset=utf-8", CONTROL_PAGE); });
  http.onNotFound([] {
    if (http.method() == HTTP_OPTIONS && http.uri().startsWith("/api/v1/rail/")) {
      http.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      http.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      http.send(204); return;
    }
    http.send(404, "application/json", "{\"error\":\"not found\"}");
  });
  http.begin();
  for (;;) { http.handleClient(); vTaskDelay(pdMS_TO_TICKS(2)); }
}

void processSerialLine(const String& text) {
  if (text == "access") {
    StaticJsonDocument<512> doc;
    doc["ssid"] = apName; doc["password"] = apPassword;
    serializeJson(doc, Serial); Serial.println(); return;
  }
  if (text == "?" || text == "status") { printStatus(); return; }
  if (text == "s" || text == "stop") {
    setLink(false);
    xQueueReset(commandQueue);
    localStop();
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
    return;
  }
  if (type == WStype_CONNECTED) {
    setLink(false);
    StaticJsonDocument<256> hello;
    hello["type"] = "hello";
    hello["device_id"] = cfg.deviceId;
    hello["simulated"] = false;
    auto axes = hello.createNestedArray("axes");
    axes.add("x");
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
    if (cmd.axis != 0) { report(id, "failed", "Only axis x has a connected L9110S motor"); return; }
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
  char randomKey[33];
  snprintf(randomKey, sizeof(randomKey), "%08x%08x", esp_random(), esp_random());
  apPassword = randomKey;
  if (preferences.begin("rail-dc", false)) {
    apPassword = preferences.getString("ap-password", apPassword);
    if (!preferences.putString("ap-password", apPassword))
      Serial.println("Local access credentials could not be persisted");
    preferences.end();
  } else {
    Serial.println("Local access credentials are temporary: NVS unavailable");
  }
  apName = "Rail-ESP32-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
    if (event == ARDUINO_EVENT_WIFI_AP_START) { ++apStarts; Serial.println("Wi-Fi AP started"); }
    if (event == ARDUINO_EVENT_WIFI_AP_STOP) { ++apStops; Serial.println("Wi-Fi AP stopped"); }
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) Serial.println("Wi-Fi client connected");
    if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.println("Wi-Fi client disconnected");
  });
  WiFi.persistent(false);
  if (!WiFi.mode(configured ? WIFI_AP_STA : WIFI_AP)) Serial.println("Wi-Fi mode setup failed");
  WiFi.setSleep(false);
  const IPAddress apIp(192,168,4,1), mask(255,255,255,0), leaseStart(192,168,4,2);
  if (!WiFi.softAPConfig(apIp, apIp, mask, leaseStart)) Serial.println("AP IP/DHCP setup failed");
  if (!WiFi.softAP(apName.c_str(), apPassword.c_str(), 1, 0, 4))
    Serial.println("Access point startup failed");
  if (esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20) != ESP_OK)
    Serial.println("AP 20 MHz setup failed");
  Serial.println("HTTP API enabled. Always-on AP; serial 'access' shows saved credentials.");
  if (xTaskCreatePinnedToCore(httpTask, "rail-http", 10000, nullptr, 1, nullptr, 0) != pdPASS)
    Serial.println("HTTP task failed to start");
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
  // L9110S digital inputs and all former driver outputs start LOW.
  for (uint8_t pin : {13,14,17,18,19,21,22,25,26,27,32}) {
    digitalWrite(pin, LOW); pinMode(pin, OUTPUT);
  }
  Serial.begin(115200);
  rmt_config_t rgbConfig = RMT_DEFAULT_CONFIG_TX(GPIO_NUM_16, RMT_CHANNEL_0);
  rgbConfig.clk_div = 2;
  rgbReady = rmt_config(&rgbConfig) == ESP_OK &&
             rmt_driver_install(RMT_CHANNEL_0, 0, 0) == ESP_OK;
  if (rgbReady) {
    // Short power-on color check, with all motor outputs still disabled.
    showRgb(40, 0, 0); delay(250);
    showRgb(0, 40, 0); delay(250);
    showRgb(0, 0, 40); delay(250);
    showRgb(0, 0, 0);
  } else Serial.println("RGB LED initialization failed");
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
  if (!linkHealthy()) controller.stopAll("Connection lost or heartbeat timeout", true);
  if (uxQueueSpacesAvailable(eventQueue) < 8) {
    controller.stopAll("Event queue congested");
    xQueueReset(commandQueue);
  }
  rail::Command command;
  for (unsigned i = 0; i < 4 && xQueueReceive(commandQueue, &command, 0) == pdTRUE; ++i) {
    if (command.stop || command.local || linkHealthy()) controller.apply(command, millis(), utcMs());
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
    snapshot.rgb[i] = axis.active && (axis.direction > 0 || ((elapsed / 150) % 2 == 0)) ? 40 : 0;
  }
  showRgb(snapshot.rgb[0], snapshot.rgb[1], snapshot.rgb[2]);
  xQueueOverwrite(snapshotQueue, &snapshot);
  delay(1);
}

