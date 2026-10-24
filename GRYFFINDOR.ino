// ============================================================
//  GRYFFINDOR.ino
//  Industrial Battery Management SCADA System
//  Hardware: ESP32-WROOM-32
//  Framework: Arduino IDE (ESP32 Arduino Core)
//
//  Required Libraries (install via Arduino Library Manager):
//    • ESPAsyncWebServer  (ESP32Async/ESPAsyncWebServer or me-no-dev fork)
//    • AsyncTCP           (ESP32Async/AsyncTCP or me-no-dev fork)
//    • ArduinoJson        (Benoit Blanchon) >= v6
//    • ESP32 Arduino Core (Espressif)
//      -- includes: Preferences, WiFi, esp_task_wdt
//
//  NOTE: This firmware deliberately avoids AsyncCallbackJsonWebHandler
//  (declared in the optional AsyncJson.h header) because that class
//  is missing or behaves inconsistently across different
//  ESPAsyncWebServer library forks/versions, which caused a
//  "was not declared in this scope" compile error. POST body
//  parsing is instead done manually via a raw body handler -- this
//  only requires ESPAsyncWebServer.h and ArduinoJson.h, which are
//  both stable across all forks.
//
//  All application configuration is in config.h
//  The web dashboard HTML/JS/CSS is in dashboard.h (PROGMEM)
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "dashboard.h"

// ─────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────
#define WDT_TIMEOUT_S      10     // Watchdog timeout in seconds
#define WIFI_RETRY_MS      30000  // Retry WiFi if disconnected

// ─────────────────────────────────────────────────────────────
//  System State Enum
// ─────────────────────────────────────────────────────────────
enum class SysState : uint8_t {
  STANDBY = 0,
  CHARGING,
  FULLY_CHARGED,
  BAT_NOT_CONNECTED,
  FAULT
};

// ─────────────────────────────────────────────────────────────
//  Event Log Entry
// ─────────────────────────────────────────────────────────────
struct LogEntry {
  char ts[20];    // "HH:MM:SS DD/MM/YY"
  char type[16];  // "INFO","WARN","ERROR","OK"
  char msg[48];   // Human-readable message
};

// ─────────────────────────────────────────────────────────────
//  Runtime Tuning Parameters (loaded from NVS at boot)
// ─────────────────────────────────────────────────────────────
struct TuneParams {
  float batFull;   // V
  float batLow;    // V
  float convMax;   // V
  float maxCurr;   // A
  int   buzInt;    // seconds
  float vs1Cal;    // multiplier
  float vs2Cal;    // multiplier
  float acsCal;    // zero-offset correction (V)
};

// ─────────────────────────────────────────────────────────────
//  Global Objects
// ─────────────────────────────────────────────────────────────
AsyncWebServer  server(80);
Preferences     prefs;

// ─── Sensor readings (updated every SENSOR_POLL_MS) ──────────
volatile float  gBatV       = 0.0f;
volatile float  gConvV      = 0.0f;
volatile float  gCurrentA   = 0.0f;
volatile float  gBatPct     = 0.0f;
volatile bool   gRelayOn    = false;
volatile SysState gState    = SysState::STANDBY;
volatile bool   gWifiOk     = false;
volatile int32_t gRSSI      = 0;

// ─── Timing ───────────────────────────────────────────────────
unsigned long   tLastSensor  = 0;
unsigned long   tLastLogic   = 0;
unsigned long   tLastWifi    = 0;
unsigned long   tLastBuzz    = 0;

// ─── Buzzer state ─────────────────────────────────────────────
bool            gBuzzerActive = false;
unsigned long   tBuzzOff      = 0;

// ─── Tuning (runtime) ─────────────────────────────────────────
TuneParams      tune;

// ─── WiFi credentials ─────────────────────────────────────────
char            cfgSSID[33];
char            cfgPass[65];

// ─── Event log (circular buffer) ──────────────────────────────
LogEntry        logBuf[LOG_MAX_ENTRIES];
uint8_t         logHead  = 0;    // next write index
uint8_t         logCount = 0;    // total entries (≤ LOG_MAX_ENTRIES)

// ─── Previous state for edge detection ─────────────────────────
SysState        prevState = SysState::STANDBY;

// ─── POST body assembly buffer (for /api/save) ──────────────────
// AsyncWebServer may deliver the POST body in multiple chunks;
// this buffer reassembles them before parsing as JSON.
#define POST_BODY_MAX  512  // max JSON payload size (excludes null terminator)
char            postBodyBuf[POST_BODY_MAX + 1]; // +1 for null terminator
size_t          postBodyLen = 0;

// ─────────────────────────────────────────────────────────────
//  Forward Declarations
// ─────────────────────────────────────────────────────────────
void    loadPrefs();
void    savePrefs();
void    factoryResetPrefs();
void    initHardware();
void    initWifi();
bool    reconnectWifi();
void    initWebServer();
void    taskSensors();
void    taskLogic();
void    taskBuzzer();
void    taskWifiWatch();
void    setRelay(bool on);
void    setBlueLed(bool on);
void    setGreenLed(bool on);
float   readVoltage(uint8_t pin, float divRatio, float calFactor);
float   readCurrent();
float   calcBatPct(float v);
void    appendLog(const char* type, const char* msg);
void    fmtTimestamp(char* buf, size_t len);
String  stateToString(SysState s);
void    handleApiStatus(AsyncWebServerRequest* req);
void    handleApiSettings(AsyncWebServerRequest* req);
void    handleApiSaveBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total);
void    handleApiLogs(AsyncWebServerRequest* req);
void    handleApiReboot(AsyncWebServerRequest* req);
void    handleApiFactoryReset(AsyncWebServerRequest* req);
void    handleApiApply(AsyncWebServerRequest* req);

// ─────────────────────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200); // Allow USB serial to stabilise
  Serial.printf("\n\n[GRYFFINDOR] Firmware v%s booting...\n", FW_VERSION);

  // ── Watchdog ────────────────────────────────────────────────
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  // ── Hardware GPIO ────────────────────────────────────────────
  initHardware();

  // ── NVS Preferences ─────────────────────────────────────────
  loadPrefs();

  // ── WiFi ─────────────────────────────────────────────────────
  initWifi();

  // ── Async Web Server ─────────────────────────────────────────
  initWebServer();

  Serial.println("[GRYFFINDOR] Setup complete.");
  appendLog("INFO", "System booted");
}

// ─────────────────────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();

  unsigned long now = millis();

  if (now - tLastSensor >= SENSOR_POLL_MS) {
    tLastSensor = now;
    taskSensors();
  }

  if (now - tLastLogic >= LOGIC_EVAL_MS) {
    tLastLogic = now;
    taskLogic();
  }

  taskBuzzer();
  taskWifiWatch();
}

// ─────────────────────────────────────────────────────────────
//  Hardware Initialisation
// ─────────────────────────────────────────────────────────────
void initHardware() {
  // Output pins
  pinMode(PIN_RELAY,      OUTPUT); digitalWrite(PIN_RELAY,     LOW);
  pinMode(PIN_BUZZER,     OUTPUT); digitalWrite(PIN_BUZZER,    LOW);
  pinMode(PIN_LED_GREEN,  OUTPUT); digitalWrite(PIN_LED_GREEN, LOW);
  pinMode(PIN_LED_BLUE,   OUTPUT); digitalWrite(PIN_LED_BLUE,  LOW);

  // ADC input pins (ESP32 ADC — no explicit pinMode needed for ADC mode)
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // full-scale ~3.3 V

  // UART2 (optional telemetry port)
  Serial2.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  Serial.println("[HW] GPIO configured.");
}

// ─────────────────────────────────────────────────────────────
//  WiFi Initialisation
// ─────────────────────────────────────────────────────────────
void initWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSSID, cfgPass);
  Serial.printf("[WiFi] Connecting to SSID: %s\n", cfgSSID);

  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); // allowed only in setup
    Serial.print('.');
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    gWifiOk = true;
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    appendLog("INFO", "WiFi connected");
  } else {
    Serial.println("\n[WiFi] Could not connect — will retry.");
    appendLog("WARN", "WiFi not connected");
  }
}

// ─────────────────────────────────────────────────────────────
//  WiFi Watchdog (called from loop via taskWifiWatch)
// ─────────────────────────────────────────────────────────────
bool reconnectWifi() {
  if (WiFi.status() == WL_CONNECTED) { gWifiOk = true; return true; }
  gWifiOk = false;
  WiFi.disconnect(false);
  WiFi.begin(cfgSSID, cfgPass);
  return false;
}

void taskWifiWatch() {
  unsigned long now = millis();
  if (now - tLastWifi < WIFI_RETRY_MS) return;
  tLastWifi = now;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Reconnecting...");
    reconnectWifi();
  } else {
    gWifiOk = true;
    gRSSI   = WiFi.RSSI();
  }
  gRSSI = WiFi.RSSI();
}

// ─────────────────────────────────────────────────────────────
//  NVS Preferences
// ─────────────────────────────────────────────────────────────
void loadPrefs() {
  prefs.begin(NVS_NAMESPACE, false);

  // WiFi credentials
  String ssid = prefs.getString(NVS_WIFI_SSID, DEFAULT_WIFI_SSID);
  String pass = prefs.getString(NVS_WIFI_PASS, DEFAULT_WIFI_PASS);
  strlcpy(cfgSSID, ssid.c_str(), sizeof(cfgSSID));
  strlcpy(cfgPass, pass.c_str(), sizeof(cfgPass));

  // Tuning
  tune.batFull = prefs.getFloat(NVS_BAT_FULL,  BAT_FULL_VOLTAGE);
  tune.batLow  = prefs.getFloat(NVS_BAT_LOW,   BAT_LOW_VOLTAGE);
  tune.convMax = prefs.getFloat(NVS_CONV_MAX,   CONV_MAX_VOLTAGE);
  tune.maxCurr = prefs.getFloat(NVS_MAX_CURRENT,MAX_CHARGE_CURRENT);
  tune.buzInt  = prefs.getInt  (NVS_BUZZER_INT, BUZZER_INTERVAL_S);
  tune.vs1Cal  = prefs.getFloat(NVS_VS1_CAL,    1.0f);
  tune.vs2Cal  = prefs.getFloat(NVS_VS2_CAL,    1.0f);
  tune.acsCal  = prefs.getFloat(NVS_ACS_CAL,    ACS712_ZERO_OFFSET);

  prefs.end();
  Serial.println("[NVS] Preferences loaded.");
}

void savePrefs() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_WIFI_SSID, cfgSSID);
  prefs.putString(NVS_WIFI_PASS, cfgPass);
  prefs.putFloat (NVS_BAT_FULL,  tune.batFull);
  prefs.putFloat (NVS_BAT_LOW,   tune.batLow);
  prefs.putFloat (NVS_CONV_MAX,  tune.convMax);
  prefs.putFloat (NVS_MAX_CURRENT, tune.maxCurr);
  prefs.putInt   (NVS_BUZZER_INT, tune.buzInt);
  prefs.putFloat (NVS_VS1_CAL,   tune.vs1Cal);
  prefs.putFloat (NVS_VS2_CAL,   tune.vs2Cal);
  prefs.putFloat (NVS_ACS_CAL,   tune.acsCal);
  prefs.end();
  Serial.println("[NVS] Preferences saved.");
}

void factoryResetPrefs() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  Serial.println("[NVS] Factory reset done.");
}

// ─────────────────────────────────────────────────────────────
//  Sensor Reading Task
// ─────────────────────────────────────────────────────────────
void taskSensors() {
  gBatV    = readVoltage(PIN_VS1, VS1_DIVIDER_RATIO, tune.vs1Cal);
  gConvV   = readVoltage(PIN_VS2, VS2_DIVIDER_RATIO, tune.vs2Cal);
  gCurrentA = readCurrent();
  gBatPct  = calcBatPct(gBatV);
}

// Oversample ADC, convert to real voltage via divider ratio
float readVoltage(uint8_t pin, float divRatio, float calFactor) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(pin);
  }
  float adcAvg  = (float)sum / ADC_SAMPLES;
  float adcVolt = (adcAvg / ADC_RESOLUTION) * ADC_VREF;
  return adcVolt * divRatio * calFactor;
}

// ACS712: Vout = Vzero + I * sensitivity
float readCurrent() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(PIN_ACS712);
  }
  float adcAvg   = (float)sum / ADC_SAMPLES;
  float adcVolt  = (adcAvg / ADC_RESOLUTION) * ADC_VREF;
  float current  = (adcVolt - tune.acsCal) / ACS712_SENSITIVITY;
  if (current < 0.0f) current = 0.0f; // clamp negative (no discharge measurement)
  return current;
}

// Linear SOC estimate between BAT_LOW_VOLTAGE and BAT_FULL_VOLTAGE
float calcBatPct(float v) {
  if (v <= tune.batLow)  return 0.0f;
  if (v >= tune.batFull) return 100.0f;
  return ((v - tune.batLow) / (tune.batFull - tune.batLow)) * 100.0f;
}

// ─────────────────────────────────────────────────────────────
//  Control Logic Task (state machine)
// ─────────────────────────────────────────────────────────────
void taskLogic() {
  SysState newState = SysState::STANDBY;

  // ── Logic 5: Over-range / sensor fault ──────────────────────
  if (gBatV > 25.0f || gConvV > 30.0f || gConvV < 0.0f || gBatV < 0.0f
      || gCurrentA > tune.maxCurr) {
    newState = SysState::FAULT;
  }
  // ── Logic 4: Battery not connected ──────────────────────────
  else if (gConvV > 15.0f && gBatV < BAT_PRESENT_MIN) {
    newState = SysState::BAT_NOT_CONNECTED;
  }
  // ── Logic 3: Battery full ────────────────────────────────────
  else if (gBatV >= tune.batFull) {
    newState = SysState::FULLY_CHARGED;
  }
  // ── Logic 2: Charging ────────────────────────────────────────
  else if (gBatV > BAT_PRESENT_MIN
           && gConvV > CONV_PRESENT_MIN
           && gConvV > gBatV
           && gCurrentA > 0.05f
           && gBatV < tune.batFull) {
    newState = SysState::CHARGING;
  }
  // ── Logic 1: Both connected but no active charge ─────────────
  else if (gBatV > BAT_PRESENT_MIN && gConvV > CONV_PRESENT_MIN) {
    newState = SysState::STANDBY;
  }
  // ── Default: fault ───────────────────────────────────────────
  else {
    newState = SysState::FAULT;
  }

  // Apply state outputs
  switch (newState) {
    case SysState::CHARGING:
      setRelay(true);
      setBlueLed(true);
      setGreenLed(false);
      gBuzzerActive = false;
      break;

    case SysState::FULLY_CHARGED:
      setRelay(false);
      setBlueLed(false);
      setGreenLed(true);
      gBuzzerActive = false;
      break;

    case SysState::BAT_NOT_CONNECTED:
      setRelay(false);
      setBlueLed(false);
      setGreenLed(false);
      gBuzzerActive = true; // pulse in taskBuzzer()
      break;

    case SysState::FAULT:
      setRelay(false);
      setBlueLed(false);
      setGreenLed(false);
      gBuzzerActive = true;
      break;

    case SysState::STANDBY:
    default:
      setRelay(false);
      setBlueLed(false);
      setGreenLed(false);
      gBuzzerActive = false;
      break;
  }

  // Edge: log state transitions
  if (newState != prevState) {
    String msg = "State → " + stateToString(newState);
    const char* logType = "INFO";
    if (newState == SysState::FAULT)             logType = "ERROR";
    else if (newState == SysState::BAT_NOT_CONNECTED) logType = "WARN";
    else if (newState == SysState::FULLY_CHARGED) logType = "OK";
    appendLog(logType, msg.c_str());
    prevState = newState;
  }

  gState = newState;
}

// ─────────────────────────────────────────────────────────────
//  Buzzer Pulse Task (non-blocking)
// ─────────────────────────────────────────────────────────────
void taskBuzzer() {
  unsigned long now = millis();

  if (!gBuzzerActive) {
    // Ensure buzzer is off when not needed
    if (digitalRead(PIN_BUZZER)) {
      digitalWrite(PIN_BUZZER, LOW);
    }
    return;
  }

  // Check if a pulse is currently active and should end
  if (digitalRead(PIN_BUZZER) == HIGH && now >= tBuzzOff) {
    digitalWrite(PIN_BUZZER, LOW);
    return;
  }

  // Trigger next pulse after interval
  if (digitalRead(PIN_BUZZER) == LOW
      && now - tLastBuzz >= (unsigned long)(tune.buzInt * 1000UL)) {
    digitalWrite(PIN_BUZZER, HIGH);
    tLastBuzz = now;
    tBuzzOff  = now + BUZZER_PULSE_MS;
  }
}

// ─────────────────────────────────────────────────────────────
//  Output Helpers
// ─────────────────────────────────────────────────────────────
void setRelay(bool on) {
  if ((bool)digitalRead(PIN_RELAY) != on) {
    digitalWrite(PIN_RELAY, on ? HIGH : LOW);
    gRelayOn = on;
    appendLog("INFO", on ? "Relay ON" : "Relay OFF");
  }
  gRelayOn = on;
}

void setBlueLed(bool on)  { digitalWrite(PIN_LED_BLUE,  on ? HIGH : LOW); }
void setGreenLed(bool on) { digitalWrite(PIN_LED_GREEN, on ? HIGH : LOW); }

// ─────────────────────────────────────────────────────────────
//  State → String
// ─────────────────────────────────────────────────────────────
String stateToString(SysState s) {
  switch (s) {
    case SysState::CHARGING:          return "Charging";
    case SysState::FULLY_CHARGED:     return "Fully Charged";
    case SysState::BAT_NOT_CONNECTED: return "Battery Not Connected";
    case SysState::FAULT:             return "Fault";
    default:                          return "Standby";
  }
}

// ─────────────────────────────────────────────────────────────
//  Event Log
// ─────────────────────────────────────────────────────────────
void fmtTimestamp(char* buf, size_t len) {
  uint32_t sec = millis() / 1000;
  uint32_t h   = (sec / 3600) % 24;
  uint32_t m   = (sec % 3600) / 60;
  uint32_t s   = sec % 60;
  snprintf(buf, len, "%02u:%02u:%02u +%lus", h, m, s, (unsigned long)sec);
}

void appendLog(const char* type, const char* msg) {
  LogEntry& e = logBuf[logHead];
  fmtTimestamp(e.ts, sizeof(e.ts));
  strlcpy(e.type, type, sizeof(e.type));
  strlcpy(e.msg,  msg,  sizeof(e.msg));
  logHead = (logHead + 1) % LOG_MAX_ENTRIES;
  if (logCount < LOG_MAX_ENTRIES) logCount++;

  Serial.printf("[LOG][%s] %s\n", type, msg);
}

// ─────────────────────────────────────────────────────────────
//  Async Web Server Setup
// ─────────────────────────────────────────────────────────────
void initWebServer() {
  // ── Serve dashboard ─────────────────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });

  // ── API: status ─────────────────────────────────────────────
  server.on("/api/status", HTTP_GET, handleApiStatus);

  // ── API: settings (GET) ──────────────────────────────────────
  server.on("/api/settings", HTTP_GET, handleApiSettings);

  // ── API: save (POST, JSON body) ──────────────────────────────
  // Implemented with a manual body-buffer handler instead of
  // AsyncCallbackJsonWebHandler (that class lives in the optional
  // AsyncJson.h header, which is missing or inconsistently shipped
  // across different ESPAsyncWebServer library forks/versions).
  // This version only depends on ESPAsyncWebServer.h + ArduinoJson.h,
  // both already included above, so it compiles on every fork.
  server.on(
    "/api/save",
    HTTP_POST,
    [](AsyncWebServerRequest* req) {
      // No-op: AsyncWebServer requires this overload to exist.
      // The real work happens once the full body has arrived,
      // in handleApiSaveBody() below.
    },
    NULL,
    handleApiSaveBody
  );

  // ── API: apply (POST) — reloads tuning without reboot ────────
  server.on("/api/apply", HTTP_POST, handleApiApply);

  // ── API: logs (GET) ──────────────────────────────────────────
  server.on("/api/logs", HTTP_GET, handleApiLogs);

  // ── API: reboot ──────────────────────────────────────────────
  server.on("/api/reboot", HTTP_POST, handleApiReboot);

  // ── API: factory reset ───────────────────────────────────────
  server.on("/api/factoryreset", HTTP_POST, handleApiFactoryReset);

  // ── 404 ──────────────────────────────────────────────────────
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "application/json", "{\"error\":\"Not found\"}");
  });

  server.begin();
  Serial.println("[WEB] Async server started on port 80.");
}

// ─────────────────────────────────────────────────────────────
//  API Handlers
// ─────────────────────────────────────────────────────────────

// GET /api/status
void handleApiStatus(AsyncWebServerRequest* req) {
  // Use a DynamicJsonDocument so it lives on the heap, not the stack,
  // keeping ISR/async stack usage low.
  DynamicJsonDocument doc(512);

  doc["batV"]     = serialized(String(gBatV,   2));
  doc["convV"]    = serialized(String(gConvV,  2));
  doc["current"]  = serialized(String(gCurrentA, 2));
  doc["batPct"]   = serialized(String(gBatPct, 1));
  doc["relayOn"]  = gRelayOn;
  doc["status"]   = stateToString(gState);
  doc["rssi"]     = gRSSI;
  doc["wifiOk"]   = gWifiOk;
  doc["wifiSSID"] = cfgSSID;
  doc["uptime"]   = millis() / 1000;
  doc["fw"]       = FW_VERSION;
  doc["device"]   = DEVICE_NAME;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["buzzerOn"] = gBuzzerActive;
  doc["fault"]    = (gState == SysState::FAULT);

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// GET /api/settings
void handleApiSettings(AsyncWebServerRequest* req) {
  DynamicJsonDocument doc(512);

  doc["batFull"]  = tune.batFull;
  doc["batLow"]   = tune.batLow;
  doc["convMax"]  = tune.convMax;
  doc["maxCurr"]  = tune.maxCurr;
  doc["buzInt"]   = tune.buzInt;
  doc["vs1Cal"]   = tune.vs1Cal;
  doc["vs2Cal"]   = tune.vs2Cal;
  doc["acsCal"]   = tune.acsCal;
  doc["wifiSSID"] = cfgSSID;
  doc["fw"]       = FW_VERSION;

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// POST /api/save — body arrives in one or more chunks; reassemble then parse.
void handleApiSaveBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
  // Guard against overflow / oversized payloads.
  if (total >= POST_BODY_MAX) {
    req->send(413, "application/json", "{\"ok\":false,\"error\":\"Payload too large\"}");
    return;
  }

  // Copy this chunk into the assembly buffer at the correct offset.
  if (index + len <= POST_BODY_MAX) {
    memcpy(postBodyBuf + index, data, len);
  }
  postBodyLen = index + len;

  // Only process once the final chunk has arrived.
  if (postBodyLen != total) return;

  postBodyBuf[postBodyLen] = '\0'; // null-terminate for ArduinoJson

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, postBodyBuf, postBodyLen);
  postBodyLen = 0; // reset for next request

  if (err) {
    req->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  JsonObject obj = doc.as<JsonObject>();

  // Tuning params
  if (obj.containsKey("batFull")) tune.batFull = obj["batFull"].as<float>();
  if (obj.containsKey("batLow"))  tune.batLow  = obj["batLow"].as<float>();
  if (obj.containsKey("convMax")) tune.convMax = obj["convMax"].as<float>();
  if (obj.containsKey("maxCurr")) tune.maxCurr = obj["maxCurr"].as<float>();
  if (obj.containsKey("buzInt"))  tune.buzInt  = obj["buzInt"].as<int>();
  if (obj.containsKey("vs1Cal"))  tune.vs1Cal  = obj["vs1Cal"].as<float>();
  if (obj.containsKey("vs2Cal"))  tune.vs2Cal  = obj["vs2Cal"].as<float>();
  if (obj.containsKey("acsCal"))  tune.acsCal  = obj["acsCal"].as<float>();

  // WiFi
  if (obj.containsKey("wifiSSID")) {
    strlcpy(cfgSSID, obj["wifiSSID"].as<const char*>(), sizeof(cfgSSID));
  }
  if (obj.containsKey("wifiPass")) {
    const char* p = obj["wifiPass"].as<const char*>();
    if (p && strlen(p) > 0) {
      strlcpy(cfgPass, p, sizeof(cfgPass));
    }
  }

  savePrefs();
  appendLog("INFO", "Settings saved via web");

  req->send(200, "application/json", "{\"ok\":true}");
}

// POST /api/apply
void handleApiApply(AsyncWebServerRequest* req) {
  // Tuning already updated in memory by /api/save; this endpoint
  // acknowledges re-evaluation will pick up the new values.
  appendLog("INFO", "Parameters applied (live)");
  req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/logs  — returns newest entries first
void handleApiLogs(AsyncWebServerRequest* req) {
  DynamicJsonDocument doc(LOG_MAX_ENTRIES * 80 + 64);
  JsonArray arr = doc.createNestedArray("logs");

  uint8_t count = logCount;
  // Walk backward from logHead - 1 to get newest-first ordering
  for (uint8_t i = 0; i < count; i++) {
    int8_t idx = ((int16_t)logHead - 1 - i + LOG_MAX_ENTRIES) % LOG_MAX_ENTRIES;
    JsonObject e = arr.createNestedObject();
    e["ts"]   = logBuf[idx].ts;
    e["type"] = logBuf[idx].type;
    e["msg"]  = logBuf[idx].msg;
  }

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// POST /api/reboot
void handleApiReboot(AsyncWebServerRequest* req) {
  req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Rebooting...\"}");
  appendLog("INFO", "Reboot requested via web");
  // Schedule reboot on next loop() cycle to allow response to flush
  delay(300);
  ESP.restart();
}

// POST /api/factoryreset
void handleApiFactoryReset(AsyncWebServerRequest* req) {
  req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Factory reset done. Rebooting...\"}");
  appendLog("WARN", "Factory reset via web");
  delay(300);
  factoryResetPrefs();
  ESP.restart();
}
