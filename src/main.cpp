#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <LittleFS.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DallasTemperature.h>
#include <time.h>

namespace {
constexpr char AP_SSID[] = "AC_Controller_Setup";
constexpr char AP_PASSWORD[] = "12345678";
constexpr char MQTT_CLIENT_ID[] = "ESP32_AC_Controller";

constexpr uint8_t ONE_WIRE_BUS_PIN = 4;
constexpr uint8_t DHT_PIN = 16;
constexpr uint8_t DHT_TYPE = DHT22;
constexpr uint8_t POWER_RELAY_PIN = 26;
constexpr uint8_t COMPRESSOR_RELAY_PIN = 27;
constexpr uint8_t FAN_PWM_PIN = 25;
constexpr uint8_t FAN_PWM_CHANNEL = 0;
constexpr uint32_t FAN_PWM_FREQ = 25000;
constexpr uint8_t FAN_PWM_RESOLUTION = 8;

constexpr unsigned long SENSOR_READ_INTERVAL_MS = 3000UL;
constexpr unsigned long TIMER_CHECK_INTERVAL_MS = 1000UL;
constexpr unsigned long LCD_REFRESH_INTERVAL_MS = 1000UL;
constexpr unsigned long LCD_PAGE_INTERVAL_MS = 30000UL;
constexpr int GMT_OFFSET_SECONDS = 19800;

struct DeviceConfig {
  String ssid;
  String password;
  String brokerUrl;
  uint16_t mqttPort = 8883;
  String mqttUser;
  String mqttPass;
  String topic = "home/ac1";
  String remoteUrl;
};

struct TimerConfig {
  int onHour = 8;
  int onMinute = 0;
  int offHour = 18;
  int offMinute = 0;
  bool valid = true;
};

WebServer server(80);
Preferences prefs;
WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);
OneWire oneWire(ONE_WIRE_BUS_PIN);
DallasTemperature ds18b20(&oneWire);
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 20, 4);

DeviceConfig config;
TimerConfig timerConfig;

float temperatures[5] = {NAN, NAN, NAN, NAN, NAN};
float dhtHumidity = NAN;
bool powerRelayState = false;
bool compressorRelayState = false;
uint8_t fanSpeedPercent = 0;
bool apMode = false;
bool disableAPAfterWiFiConnect = false;
unsigned long lastSensorReadMs = 0;
unsigned long lastTimerCheckMs = 0;
unsigned long lastLcdRefreshMs = 0;
unsigned long lcdPageStartedMs = 0;
int lcdPageIndex = 0;
int lastTimerFiredMinute = -1;
bool timerOnFiredToday = false;
bool timerOffFiredToday = false;
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastMQTTAttemptMs = 0;
unsigned long lastMQTTPublishMs = 0;

constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000UL;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000UL;
constexpr unsigned long MQTT_STATUS_INTERVAL_MS = 15000UL;

void publishStatus();

bool isValidTemperature(float value) {
  return !isnan(value) && value > -80.0f && value < 150.0f;
}

String formatTimeValue(int hour, int minute) {
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
  return String(buffer);
}

bool parseTimeString(const String &value, int &hour, int &minute) {
  if (value.length() != 5 || value.charAt(2) != ':') {
    return false;
  }

  hour = value.substring(0, 2).toInt();
  minute = value.substring(3, 5).toInt();
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

uint16_t parsePortValue(const JsonVariantConst &value, uint16_t fallback = 8883) {
  if (value.is<uint16_t>()) {
    return value.as<uint16_t>();
  }

  if (value.is<int>()) {
    const int parsed = value.as<int>();
    return parsed > 0 && parsed <= 65535 ? static_cast<uint16_t>(parsed) : fallback;
  }

  if (value.is<const char *>()) {
    String text = value.as<const char *>();
    text.trim();
    const int parsed = text.toInt();
    return parsed > 0 && parsed <= 65535 ? static_cast<uint16_t>(parsed) : fallback;
  }

  return fallback;
}

String normalizedBrokerHost(String value) {
  value.trim();
  value.replace("mqtt://", "");
  value.replace("mqtts://", "");
  value.replace("ssl://", "");
  value.replace("tcp://", "");
  value.trim();

  int slashIndex = value.indexOf('/');
  if (slashIndex >= 0) {
    value = value.substring(0, slashIndex);
  }

  int colonIndex = value.indexOf(':');
  if (colonIndex >= 0) {
    value = value.substring(0, colonIndex);
  }

  value.trim();
  return value;
}

String baseTopic() {
  String topic = config.topic;
  topic.trim();
  if (topic.isEmpty()) {
    topic = "home/ac1";
  }
  while (topic.endsWith("/")) {
    topic.remove(topic.length() - 1);
  }
  return topic;
}

String controlTopic() { return baseTopic() + "/control"; }
String fanSetTopic() { return baseTopic() + "/fan/set"; }
String timerSetTopic() { return baseTopic() + "/timer/set"; }
String statusTopic() { return baseTopic() + "/status"; }
String statusGetTopic() { return baseTopic() + "/status/get"; }
String availabilityTopic() { return baseTopic() + "/availability"; }

bool mqttConfigured() {
  return normalizedBrokerHost(config.brokerUrl).length() > 0 && baseTopic().length() > 0;
}

void sendJsonResponse(int code, const JsonDocument &doc) {
  String payload;
  serializeJson(doc, payload);
  server.send(code, "application/json", payload);
}

bool parseIncomingJson(JsonDocument &doc) {
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) {
    JsonDocument response;
    response["error"] = "Invalid JSON payload";
    sendJsonResponse(400, response);
    return false;
  }

  return true;
}

void loadConfig() {
  prefs.begin("config", true);
  config.ssid = prefs.getString("ssid", "");
  config.password = prefs.getString("pass", "");
  config.brokerUrl = prefs.getString("broker", "");
  config.mqttPort = static_cast<uint16_t>(prefs.getUInt("port", 8883));
  config.mqttUser = prefs.getString("user", "");
  config.mqttPass = prefs.getString("mpass", "");
  config.topic = prefs.getString("topic", "home/ac1");
  config.remoteUrl = prefs.getString("remote", "");
  prefs.end();
}

void saveConfig(const JsonDocument &doc) {
  const String ssid = String(doc["ssid"] | "");
  const String password = String(doc["password"] | "");

  config.ssid = ssid;
  config.password = password;
  config.brokerUrl = String(doc["broker_url"] | "");
  config.mqttPort = parsePortValue(doc["port"], 8883);
  config.mqttUser = String(doc["mqtt_user"] | "");
  config.mqttPass = String(doc["mqtt_pass"] | "");
  config.topic = String(doc["topic"] | "home/ac1");
  config.remoteUrl = String(doc["remote_url"] | "");

  prefs.begin("config", false);
  prefs.putString("ssid", config.ssid);
  prefs.putString("pass", config.password);
  prefs.putString("broker", config.brokerUrl);
  prefs.putUInt("port", config.mqttPort);
  prefs.putString("user", config.mqttUser);
  prefs.putString("mpass", config.mqttPass);
  prefs.putString("topic", config.topic);
  prefs.putString("remote", config.remoteUrl);
  prefs.end();
}

void loadTimerConfig() {
  prefs.begin("timer", true);
  timerConfig.onHour = prefs.getInt("onHour", 8);
  timerConfig.onMinute = prefs.getInt("onMinute", 0);
  timerConfig.offHour = prefs.getInt("offHour", 18);
  timerConfig.offMinute = prefs.getInt("offMinute", 0);
  timerConfig.valid = prefs.getBool("valid", true);
  prefs.end();
}

void saveTimerConfig() {
  prefs.begin("timer", false);
  prefs.putInt("onHour", timerConfig.onHour);
  prefs.putInt("onMinute", timerConfig.onMinute);
  prefs.putInt("offHour", timerConfig.offHour);
  prefs.putInt("offMinute", timerConfig.offMinute);
  prefs.putBool("valid", timerConfig.valid);
  prefs.end();
}

bool hasWiFiCredentials() {
  return config.ssid.length() > 0 && config.password.length() > 0;
}

void startApMode() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apMode = true;
  Serial.print("AP started at ");
  Serial.println(WiFi.softAPIP());
}

void connectWiFiIfNeeded(bool forceNow = false) {
  if (!hasWiFiCredentials()) {
    if (!apMode) {
      startApMode();
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (!forceNow && millis() - lastWiFiAttemptMs < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  lastWiFiAttemptMs = millis();
  Serial.printf("Connecting to WiFi SSID: %s\n", config.ssid.c_str());
}

void maybeDisableApAfterConnect() {
  if (disableAPAfterWiFiConnect && apMode && WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);
    apMode = false;
    disableAPAfterWiFiConnect = false;
    Serial.println("Setup AP disabled after successful WiFi connection");
  }
}

void syncTimeIfNeeded() {
  static bool requested = false;
  if (requested || WiFi.status() != WL_CONNECTED) {
    return;
  }

  configTime(GMT_OFFSET_SECONDS, 0, "pool.ntp.org", "time.nist.gov");
  requested = true;
}

void applyFanSpeed(uint8_t percent) {
  fanSpeedPercent = constrain(percent, 0, 100);
  const uint8_t duty = map(fanSpeedPercent, 0, 100, 0, 255);
  ledcWrite(FAN_PWM_CHANNEL, duty);
}

void setCompressorRelay(bool state) {
  compressorRelayState = state && powerRelayState;
  digitalWrite(COMPRESSOR_RELAY_PIN, compressorRelayState ? HIGH : LOW);
}

void setPowerRelay(bool state) {
  powerRelayState = state;
  digitalWrite(POWER_RELAY_PIN, powerRelayState ? HIGH : LOW);
  if (!powerRelayState) {
    setCompressorRelay(false);
  }
}

void setControlStates(bool powerOn, bool compressorOn) {
  setPowerRelay(powerOn);
  setCompressorRelay(compressorOn);
}

void readTemperaturesIfNeeded() {
  if (millis() - lastSensorReadMs < SENSOR_READ_INTERVAL_MS) {
    return;
  }

  lastSensorReadMs = millis();
  ds18b20.requestTemperatures();

  for (int index = 0; index < 4; ++index) {
    const float reading = ds18b20.getTempCByIndex(index);
    temperatures[index] = (reading == DEVICE_DISCONNECTED_C || !isValidTemperature(reading)) ? NAN : reading;
  }

  const float dhtTemperature = dht.readTemperature();
  temperatures[4] = isValidTemperature(dhtTemperature) ? dhtTemperature : NAN;
  const float humidity = dht.readHumidity();
  dhtHumidity = (!isnan(humidity) && humidity >= 0.0f && humidity <= 100.0f) ? humidity : NAN;
}

String lineForReading(const char *label, float value) {
  char buffer[21];
  if (isValidTemperature(value)) {
    snprintf(buffer, sizeof(buffer), "%-9s %5.1f C", label, value);
  } else {
    snprintf(buffer, sizeof(buffer), "%-9s   --.- C", label);
  }
  return String(buffer);
}

void updateLcd() {
  if (millis() - lastLcdRefreshMs < LCD_REFRESH_INTERVAL_MS) {
    return;
  }

  lastLcdRefreshMs = millis();

  if (millis() - lcdPageStartedMs >= LCD_PAGE_INTERVAL_MS) {
    lcdPageStartedMs = millis();
    lcdPageIndex = (lcdPageIndex + 1) % 3;
  }

  lcd.clear();

  if (lcdPageIndex == 0) {
    lcd.setCursor(0, 0);
    lcd.print("AC Temp Monitor");
    lcd.setCursor(0, 1);
    lcd.print(lineForReading("Thermo1", temperatures[0]));
    lcd.setCursor(0, 2);
    lcd.print(lineForReading("Thermo2", temperatures[1]));
    lcd.setCursor(0, 3);
    lcd.print("Page 1/3  Next 30s");
    return;
  }

  if (lcdPageIndex == 1) {
    lcd.setCursor(0, 0);
    lcd.print("AC Temp Monitor");
    lcd.setCursor(0, 1);
    lcd.print(lineForReading("Thermo3", temperatures[2]));
    lcd.setCursor(0, 2);
    lcd.print(lineForReading("Thermo4", temperatures[3]));
    lcd.setCursor(0, 3);
    lcd.print("Page 2/3  Next 30s");
    return;
  }

  char humidityBuffer[21];
  if (!isnan(dhtHumidity)) {
    snprintf(humidityBuffer, sizeof(humidityBuffer), "Humidity  %5.1f %%", dhtHumidity);
  } else {
    snprintf(humidityBuffer, sizeof(humidityBuffer), "Humidity    --.- %%");
  }

  char stateBuffer[21];
  snprintf(
    stateBuffer,
    sizeof(stateBuffer),
    "P:%s C:%s F:%3u%%",
    powerRelayState ? "ON " : "OFF",
    compressorRelayState ? "ON " : "OFF",
    fanSpeedPercent
  );

  lcd.setCursor(0, 0);
  lcd.print("Ambient and Status");
  lcd.setCursor(0, 1);
  lcd.print(lineForReading("DHT22", temperatures[4]));
  lcd.setCursor(0, 2);
  lcd.print(String(humidityBuffer));
  lcd.setCursor(0, 3);
  lcd.print(String(stateBuffer));
}

void applyTimerValues(const String &onTime, const String &offTime) {
  int onHour = 0;
  int onMinute = 0;
  int offHour = 0;
  int offMinute = 0;

  if (!parseTimeString(onTime, onHour, onMinute) || !parseTimeString(offTime, offHour, offMinute)) {
    return;
  }

  timerConfig.onHour = onHour;
  timerConfig.onMinute = onMinute;
  timerConfig.offHour = offHour;
  timerConfig.offMinute = offMinute;
  timerConfig.valid = true;
  timerOnFiredToday = false;
  timerOffFiredToday = false;
  lastTimerFiredMinute = -1;
  saveTimerConfig();
}

void checkTimer() {
  if (!timerConfig.valid || millis() - lastTimerCheckMs < TIMER_CHECK_INTERVAL_MS) {
    return;
  }

  lastTimerCheckMs = millis();

  tm timeInfo;
  if (!getLocalTime(&timeInfo, 50)) {
    return;
  }

  const int currentMinute = (timeInfo.tm_hour * 60) + timeInfo.tm_min;
  const int onMinute = (timerConfig.onHour * 60) + timerConfig.onMinute;
  const int offMinute = (timerConfig.offHour * 60) + timerConfig.offMinute;

  if (currentMinute == 0 && lastTimerFiredMinute != 0) {
    timerOnFiredToday = false;
    timerOffFiredToday = false;
  }

  lastTimerFiredMinute = currentMinute;

  if (currentMinute == onMinute && !timerOnFiredToday) {
    timerOnFiredToday = true;
    setPowerRelay(true);
    publishStatus();
  }

  if (currentMinute == offMinute && !timerOffFiredToday) {
    timerOffFiredToday = true;
    setPowerRelay(false);
    publishStatus();
  }
}

void populateStatus(JsonDocument &response) {
  JsonArray tempArray = response["temperatures"].to<JsonArray>();
  for (float temperature : temperatures) {
    if (isValidTemperature(temperature)) {
      tempArray.add(temperature);
    } else {
      tempArray.add(nullptr);
    }
  }

  response["power_on"] = powerRelayState;
  response["compressor_on"] = compressorRelayState;
  response["fan_speed_percent"] = fanSpeedPercent;
  response["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  response["mqtt_connected"] = mqttClient.connected();
  response["mqtt_configured"] = mqttConfigured();
  response["wifi_saved"] = hasWiFiCredentials();
  response["ap_mode"] = apMode;
  response["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  response["ap_ip"] = WiFi.softAPIP().toString();
  response["sta_ip"] = WiFi.localIP().toString();
  response["topic"] = baseTopic();
  response["remote_url"] = config.remoteUrl;
  response["on_time"] = formatTimeValue(timerConfig.onHour, timerConfig.onMinute);
  response["off_time"] = formatTimeValue(timerConfig.offHour, timerConfig.offMinute);
  if (!isnan(dhtHumidity)) {
    response["dht_humidity"] = dhtHumidity;
  } else {
    response["dht_humidity"] = nullptr;
  }
}

void publishAvailability(const char *value) {
  if (!mqttClient.connected()) {
    return;
  }
  mqttClient.publish(availabilityTopic().c_str(), value, true);
}

void publishStatus() {
  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument payload;
  populateStatus(payload);
  String body;
  serializeJson(payload, body);
  mqttClient.publish(statusTopic().c_str(), body.c_str(), true);
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String topicName = String(topic);
  String message;
  message.reserve(length);
  for (unsigned int index = 0; index < length; ++index) {
    message += static_cast<char>(payload[index]);
  }

  if (topicName == controlTopic()) {
    JsonDocument request;
    if (!deserializeJson(request, message)) {
      setControlStates(request["power_on"] | powerRelayState, request["compressor_on"] | compressorRelayState);
      publishStatus();
    }
    return;
  }

  if (topicName == fanSetTopic()) {
    JsonDocument request;
    if (!deserializeJson(request, message)) {
      const int requestedPercent = request["fan_speed_percent"] | fanSpeedPercent;
      applyFanSpeed(static_cast<uint8_t>(constrain(requestedPercent, 0, 100)));
      publishStatus();
    }
    return;
  }

  if (topicName == timerSetTopic()) {
    JsonDocument request;
    if (!deserializeJson(request, message)) {
      applyTimerValues(String(request["on_time"] | ""), String(request["off_time"] | ""));
      publishStatus();
    }
    return;
  }

  if (topicName == statusGetTopic()) {
    publishStatus();
  }
}

void configureMQTTClient() {
  secureClient.setInsecure();
  mqttClient.setServer(normalizedBrokerHost(config.brokerUrl).c_str(), config.mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(10);
  mqttClient.setBufferSize(1024);
}

void connectMQTTIfNeeded(bool forceNow = false) {
  if (!mqttConfigured() || WiFi.status() != WL_CONNECTED || mqttClient.connected()) {
    return;
  }

  if (!forceNow && millis() - lastMQTTAttemptMs < MQTT_RETRY_INTERVAL_MS) {
    return;
  }

  lastMQTTAttemptMs = millis();
  configureMQTTClient();

  const String willTopic = availabilityTopic();
  bool connected = false;
  if (config.mqttUser.length() > 0) {
    connected = mqttClient.connect(
      MQTT_CLIENT_ID,
      config.mqttUser.c_str(),
      config.mqttPass.c_str(),
      willTopic.c_str(),
      0,
      true,
      "offline"
    );
  } else {
    connected = mqttClient.connect(MQTT_CLIENT_ID, willTopic.c_str(), 0, true, "offline");
  }

  if (connected) {
    mqttClient.subscribe(controlTopic().c_str());
    mqttClient.subscribe(fanSetTopic().c_str());
    mqttClient.subscribe(timerSetTopic().c_str());
    mqttClient.subscribe(statusGetTopic().c_str());
    publishAvailability("online");
    publishStatus();
  }
}

void periodicMQTTStatusPublish() {
  if (!mqttClient.connected() || millis() - lastMQTTPublishMs < MQTT_STATUS_INTERVAL_MS) {
    return;
  }

  lastMQTTPublishMs = millis();
  publishStatus();
}

void handleRoot() {
  if (!LittleFS.exists("/index.html")) {
    server.send(500, "text/plain", "index.html not found");
    return;
  }

  File file = LittleFS.open("/index.html", "r");
  server.streamFile(file, "text/html");
  file.close();
}

void handleDashboard() {
  handleRoot();
}

void handleStatus() {
  JsonDocument response;
  populateStatus(response);
  sendJsonResponse(200, response);
}

void handleSave() {
  JsonDocument request;
  if (!parseIncomingJson(request)) {
    return;
  }

  const String ssidValue = String(request["ssid"] | "");
  const String passwordValue = String(request["password"] | "");
  const String brokerValue = String(request["broker_url"] | "");
  const String topicValue = String(request["topic"] | "");
  if (ssidValue.isEmpty() || passwordValue.isEmpty()) {
    JsonDocument response;
    response["error"] = "ssid and password are required";
    sendJsonResponse(400, response);
    return;
  }
  if (brokerValue.isEmpty() || topicValue.isEmpty()) {
    JsonDocument response;
    response["error"] = "MQTT broker URL and topic are required for remote access";
    sendJsonResponse(400, response);
    return;
  }

  saveConfig(request);
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }

  JsonDocument response;
  response["ok"] = true;
  response["message"] = "WiFi and MQTT settings saved";
  response["remote_url"] = config.remoteUrl;
  sendJsonResponse(200, response);
}

void handleConnect() {
  if (!hasWiFiCredentials()) {
    JsonDocument response;
    response["error"] = "WiFi credentials are not saved";
    sendJsonResponse(400, response);
    return;
  }

  disableAPAfterWiFiConnect = true;
  connectWiFiIfNeeded(true);

  JsonDocument response;
  response["ok"] = true;
  response["message"] = "Connecting to WiFi. Stay on the same network to reopen the dashboard.";
  response["remote_url"] = config.remoteUrl;
  sendJsonResponse(200, response);
}

void handleControl() {
  JsonDocument request;
  if (!parseIncomingJson(request)) {
    return;
  }

  const bool powerOn = request["power_on"] | false;
  const bool compressorOn = request["compressor_on"] | false;
  setControlStates(powerOn, compressorOn);

  JsonDocument response;
  response["ok"] = true;
  response["power_on"] = powerRelayState;
  response["compressor_on"] = compressorRelayState;
  publishStatus();
  sendJsonResponse(200, response);
}

void handleFan() {
  JsonDocument request;
  if (!parseIncomingJson(request)) {
    return;
  }

  const int requestedPercent = request["fan_speed_percent"] | 0;
  applyFanSpeed(static_cast<uint8_t>(constrain(requestedPercent, 0, 100)));

  JsonDocument response;
  response["ok"] = true;
  response["fan_speed_percent"] = fanSpeedPercent;
  publishStatus();
  sendJsonResponse(200, response);
}

void handleTimer() {
  JsonDocument request;
  if (!parseIncomingJson(request)) {
    return;
  }

  const String onTime = String(request["on_time"] | "");
  const String offTime = String(request["off_time"] | "");
  int hour = 0;
  int minute = 0;
  if (!parseTimeString(onTime, hour, minute) || !parseTimeString(offTime, hour, minute)) {
    JsonDocument response;
    response["error"] = "Invalid time format. Use HH:MM";
    sendJsonResponse(400, response);
    return;
  }

  applyTimerValues(onTime, offTime);

  JsonDocument response;
  response["ok"] = true;
  response["on_time"] = formatTimeValue(timerConfig.onHour, timerConfig.onMinute);
  response["off_time"] = formatTimeValue(timerConfig.offHour, timerConfig.offMinute);
  publishStatus();
  sendJsonResponse(200, response);
}

void handleNotFound() {
  JsonDocument response;
  response["error"] = "Not found";
  sendJsonResponse(404, response);
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/dashboard", HTTP_GET, handleDashboard);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/control", HTTP_POST, handleControl);
  server.on("/fan", HTTP_POST, handleFan);
  server.on("/timer", HTTP_POST, handleTimer);
  server.onNotFound(handleNotFound);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(POWER_RELAY_PIN, OUTPUT);
  pinMode(COMPRESSOR_RELAY_PIN, OUTPUT);
  digitalWrite(POWER_RELAY_PIN, LOW);
  digitalWrite(COMPRESSOR_RELAY_PIN, LOW);

  ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
  ledcAttachPin(FAN_PWM_PIN, FAN_PWM_CHANNEL);
  applyFanSpeed(0);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AC Controller Boot");
  lcd.setCursor(0, 1);
  lcd.print("Starting services...");

  dht.begin();
  ds18b20.begin();

  loadConfig();
  loadTimerConfig();

  if (hasWiFiCredentials()) {
    connectWiFiIfNeeded(true);
    startApMode();
  } else {
    startApMode();
  }

  setupRoutes();
  server.begin();

  lcdPageStartedMs = millis();
  lastSensorReadMs = millis() - SENSOR_READ_INTERVAL_MS;
  readTemperaturesIfNeeded();
  updateLcd();
}

void loop() {
  server.handleClient();
  readTemperaturesIfNeeded();
  updateLcd();

  if (WiFi.status() == WL_CONNECTED) {
    syncTimeIfNeeded();
    connectMQTTIfNeeded();
    mqttClient.loop();
    checkTimer();
    periodicMQTTStatusPublish();
  } else {
    connectWiFiIfNeeded();
  }

  maybeDisableApAfterConnect();
  delay(5);
}
