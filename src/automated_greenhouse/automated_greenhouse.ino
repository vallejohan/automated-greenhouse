#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <config.h>
#include <dht11.h>

// ============= USER CONFIG =============
constexpr char WIFI_SSID[]   = SECRET_WIFI_SSID;
constexpr char WIFI_PASS[]   = SECRET_WIFI_PASS;

constexpr char MQTT_HOST[]   = "homeassistant.local";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char MQTT_USER[]   = "mqtt";
constexpr char MQTT_PASS[]   = "Trapphus123";
constexpr char DEVICE_ID[]   = "device_1";
// ----------------------------------------

// ============= TOPICS ===================
String t_base  = "garden";
String t_state = t_base + "/state";
String t_msg_base   = t_base + "/msg";
String t_msg_sub  = t_msg_base + "/#";
String t_birth = t_base + "/birth";
// ----------------------------------------

// ============= DEFAULTS =================
RTC_DATA_ATTR uint32_t wakeupIntervalHour = 0;
RTC_DATA_ATTR uint32_t wakeupIntervalMin = 0;
RTC_DATA_ATTR uint32_t wakeupIntervalSec = 5;
RTC_DATA_ATTR uint32_t pumpRunTimeSec    = 5;
RTC_DATA_ATTR uint16_t moistureThreshold = 0;
//RTC_DATA_ATTR bool     runPumpNowFlag = true;
// ----------------------------------------

// ============= OBJECTS ==================
WiFiClient   wifi;
PubSubClient mqtt(wifi);
Adafruit_INA219 ina219;
Preferences  prefs;
dht11 DHT11;

static PumpConfig pumps[] = {
  { "pump1", PUMP1_PIN, false, 30},
  { "pump2", PUMP2_PIN, false, 30}
};
static const uint8_t pumpCount = sizeof(pumps) / sizeof(pumps[0]);

float busVoltage = 0.0;
float current_mA = 0.0;
float power_mW = 0.0;
int chk = 0;
uint16_t outputState = 0;
// ----------------------------------------

/* ---------- forward declarations ---------- */
bool   connectWiFi(uint32_t timeoutMs = 10000);
bool   connectMQTT(uint32_t timeoutMs = 8000);
void   publishReading(float busVoltage, float current_mA, float power_mW, float dht11_humidity, float dht11_temp);
void   onMqttMsg(char *topic, byte *payload, unsigned int len);
void   runPump(uint16_t pumpPin, uint32_t runTimeSec);
void   loadPrefs();
void   savePrefs();

/* --------------- SETUP -------------------- */
void setup() {
  //uint8_t minutesToSleep = 0;
  // Init GPIO pins for motor driver and shift register
  initGPIOPins();

  Serial.begin(115200);
  delay(200);

  loadPrefs();

  Wire.begin(SDA_PIN, SCL_PIN);

  // Start INA219
  if (!DRY_RUN) {
    if (!ina219.begin(&Wire)) {
      printMessage("Failed to find INA219 chip");
      while (1);
    }
  }
  printMessage("INA219 initialized.");
}

void loop() {
  // Read current/voltage/power from INA219
  busVoltage = DRY_RUN ? 1.0 : ina219.getBusVoltage_V();
  current_mA = DRY_RUN ? 2.0 : ina219.getCurrent_mA();
  power_mW = DRY_RUN ? 3.0 : ina219.getPower_mW();
  if (current_mA < 0.0) {
    current_mA = 0.0;
  }
  if (power_mW < 0.0) {
    power_mW = 0.0;
  }

  int chk = DHT11.read(DHT11_PIN);

  /* Connect to Wi‑Fi and MQTT server */
  while (!connectWiFi()) {
    delay(1);
  }

  while(!connectMQTT()) {
    delay(1);
  }

  publishReading(busVoltage, current_mA, power_mW, DRY_RUN ? 30.0 : (float)DHT11.humidity, DRY_RUN ? 20.5 : (float)DHT11.temperature);

  uint32_t waitUntil = millis() + 5000;
  while (millis() < waitUntil) {
    mqtt.loop();
    delay(1);
  }

  //maybeRunPump(0);
  mqtt.disconnect();
  WiFi.disconnect(true, true);

  delay(1000);

}

void initGPIOPins() {
  pinMode(DS, OUTPUT);
  pinMode(SHCP, OUTPUT);
  pinMode(STCP, OUTPUT);

  pinMode(STBY, OUTPUT);

  pinMode(TB1_PWMA, OUTPUT);
  pinMode(TB1_PWMB, OUTPUT);
  pinMode(TB2_PWMA, OUTPUT);
  pinMode(TB2_PWMB, OUTPUT);

  // Disable pumps
  setActivePin(PUMP1_PIN, false);
  setActivePin(PUMP2_PIN, false);

  digitalWrite(STBY, LOW);

  analogWrite(TB1_PWMA, 0);
  analogWrite(TB1_PWMB, 0);
  analogWrite(TB2_PWMA, 0);
  analogWrite(TB2_PWMB, 0);

  setActivePin(TB1_AIN1, false);
  setActivePin(TB1_AIN2, false);
  setActivePin(TB1_BIN1, false);
  setActivePin(TB1_BIN2, false);
  setActivePin(TB2_AIN1, false);
  setActivePin(TB2_AIN2, false);
  setActivePin(TB2_BIN1, false);
  setActivePin(TB2_BIN2, false);
}

void printMessage(const String& msg) {
  if (DEBUG_OUTPUT) {
    Serial.println(msg);
  }
}

void setActivePin(uint8_t pin, bool state) {
  if (pin > 15) return;

  if (state) {
    outputState |=  (1 << pin);   // set bit
  } else {
    outputState &= ~(1 << pin);   // clear bit
  }

  digitalWrite(STCP, LOW);
  shiftOut(DS, SHCP, MSBFIRST, highByte(outputState)); // second 595
  shiftOut(DS, SHCP, MSBFIRST, lowByte(outputState));  // first 595
  digitalWrite(STCP, HIGH);
}

/* -------- Wi‑Fi connect -------- */
bool connectWiFi(uint32_t timeoutMs) {
  printMessage("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) return false;
    delay(50);
  }
  printMessage("Connected!");
  return true;
}

/* -------- MQTT connect -------- */
bool connectMQTT(uint32_t timeoutMs) {
  printMessage("Connecting to MQTT server");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMsg);
  mqtt.setBufferSize(256);

  String clientId = String("sensor‑") + String(random(0xffff), HEX);
  uint32_t start = millis();
  while (!mqtt.connected()) {
    if (!mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                      t_birth.c_str(), 0, true, "offline"))
    {
      if (millis() - start > timeoutMs) return false;
      delay(250);
    }
  }
  printMessage("Connected!");
  mqtt.publish(t_birth.c_str(), "online", true);   // retained birth
  mqtt.subscribe(t_msg_sub.c_str(), 0);                // QoS 0 fine here
  return true;
}

/* -------- Publish readings -------- */
void publishReading(float busVoltage, float current_mA, float power_mW, float dht11_humidity, float dht11_temp) {
  printMessage("Publish readings to MQTT server");
  char payload1[128];
  snprintf(payload1, sizeof(payload1),
           "{\"bus_v\": %.2f, \"cur_mA\": %.2f, \"pow_mW\": %.2f, \"dht_hum\": %.2f, \"dht_temp\": %.2f}",
           busVoltage, current_mA, power_mW, dht11_humidity, dht11_temp);
  Serial.println(payload1);
  mqtt.publish(t_state.c_str(), payload1);
}

/* -------- Incoming commands from MQTT server -------- */
void onMqttMsg(char *topic, byte *payload, unsigned int len) {
    String topicStr(topic);
    String basePrefix = t_msg_base + "/";
    if (!topicStr.startsWith(basePrefix)) return;

    String unique_topic = topicStr.substring(basePrefix.length());
    String topic_value = String((char*)payload, len);
    topic_value.trim();
    String valLower = topic_value;
    valLower.toLowerCase();
    printMessage("Got MQTT command: key='" + unique_topic + "' val='" + topic_value + "'");

    if (unique_topic.startsWith("pump/")) {
      String rest = unique_topic.substring(String("pump/").length());
      int slashPos = rest.indexOf('/');
      if (slashPos <= 0) {
        printMessage("Invalid pump topic: " + unique_topic);
      } else {
        String idxStr = rest.substring(0, slashPos);
        int pumpNumber = idxStr.toInt();
        if (pumpNumber < 1 || pumpNumber > pumpCount) {
          printMessage("Invalid pump index: " + idxStr);
        } else {
          int pumpIdx = pumpNumber - 1;
          uint8_t pumpPin = pumps[pumpIdx].pin;
          String field = rest.substring(slashPos + 1);
          field.toLowerCase();
          printMessage("Pump command for pump " + String(pumpNumber) + " field: " + field);
          if (field == "active") {
            bool on = (valLower == "on" || valLower == "1" || valLower == "true");
            bool off = (valLower == "off" || valLower == "0" || valLower == "false");
            if (!on && !off) {
              printMessage("Invalid pump active value: " + topic_value);
            } else {
              pumps[pumpIdx].active = on;
              setActivePin(pumpPin, on);
              printMessage("Pump " + String(pumpNumber) + " turned " + (on ? "ON" : "OFF"));
              char payload_out[128];
              snprintf(payload_out, sizeof(payload_out), "{\"pump\":%d,\"active\":%s}",
                       pumpNumber, on ? "true" : "false");
              mqtt.publish(t_state.c_str(), payload_out);
            }
          } else if (field == "run_time" || field == "run_time_sec" || field == "runtime") {
            long runTime = topic_value.toInt();
            runTime = constrain(runTime, 1, 3600);
            pumps[pumpIdx].runTimeSec = (uint32_t)runTime;
            if (pumpIdx == 0) {
              pumpRunTimeSec = (uint32_t)runTime;
            }
            char payload_out[128];
            snprintf(payload_out, sizeof(payload_out), "{\"pump\":%d,\"run_time\":%ld}",
                     pumpNumber, runTime);
            mqtt.publish(t_state.c_str(), payload_out);
            runPump(pumpPin, pumps[pumpIdx].runTimeSec);
          } else {
            printMessage("Unknown pump field: " + field);
          }
        }
      }
    } else if (unique_topic.startsWith("motor/")) {
      String idxStr = unique_topic.substring(String("motor/").length());
      int idx = idxStr.toInt();
      if (idx < 0 || idx > 3) {
        printMessage("Invalid motor index: " + idxStr);
      } else {
        static const uint8_t motorPins[4][2] = {
          { TB1_AIN1, TB1_AIN2 }, // motor 0
          { TB1_BIN1, TB1_BIN2 }, // motor 1
          { TB2_AIN1, TB2_AIN2 }, // motor 2
          { TB2_BIN1, TB2_BIN2 }  // motor 3
        };
        uint8_t a = motorPins[idx][0];
        uint8_t b = motorPins[idx][1];

        if (valLower == "forward" || valLower == "f" || valLower == "1" || valLower == "on") {
          setActivePin(a, true);
          setActivePin(b, false);
          printMessage("Motor " + String(idx) + " -> FORWARD");
        } else if (valLower == "reverse" || valLower == "r" || valLower == "-1") {
          setActivePin(a, false);
          setActivePin(b, true);
          printMessage("Motor " + String(idx) + " -> REVERSE");
        } else if (valLower == "stop" || valLower == "off" || valLower == "0") {
          setActivePin(a, false);
          setActivePin(b, false);
          printMessage("Motor " + String(idx) + " -> STOP");
        } else {
          printMessage("Unknown motor command: " + topic_value);
        }

        char payload_out[128];
        snprintf(payload_out, sizeof(payload_out), "{\"motor\":%d,\"cmd\":\"%s\"}", idx, topic_value.c_str());
        mqtt.publish(t_state.c_str(), payload_out);
      }
    } else {
      printMessage("Unknown MQTT key: '" + unique_topic + "'");
    }
    savePrefs();   // commit to flash so it survives power‑loss
}

/* -------- Pump control -------- */
void runPump(uint16_t pumpPin, uint32_t runTimeSec = 0) {
    printMessage("Starting water pump for " + String(runTimeSec) + " seconds");

    char payload[128];
    snprintf(payload, sizeof(payload),"{\"pump_active\":\"%s\"}","ON");
    mqtt.publish(t_state.c_str(), payload);
    delay(300);
    setActivePin(pumpPin, true);

    delay(runTimeSec * 1000UL);

    snprintf(payload, sizeof(payload),"{\"pump_active\":\"%s\"}","OFF");
    mqtt.publish(t_state.c_str(), payload);
    delay(300);
    setActivePin(pumpPin, false);
}

/* -------- NVS helpers -------- */
void loadPrefs() {
  if (!prefs.begin(DEVICE_ID)) return;
  wakeupIntervalMin    = prefs.getUInt("interval",    wakeupIntervalMin);
  for (uint8_t i = 0; i < pumpCount; i++) {
    char key[16];
    snprintf(key, sizeof(key), "pump%uTime", i + 1);
    pumps[i].runTimeSec = prefs.getUInt(key, pumps[i].runTimeSec);
  }
  pumpRunTimeSec = pumps[0].runTimeSec;
  moistureThreshold = prefs.getUShort("threshold", moistureThreshold);
  prefs.end();
}
void savePrefs() {
  if (!prefs.begin(DEVICE_ID)) return;
  prefs.putUInt("interval",    wakeupIntervalMin);
  for (uint8_t i = 0; i < pumpCount; i++) {
    char key[16];
    snprintf(key, sizeof(key), "pump%uTime", i + 1);
    prefs.putUInt(key, pumps[i].runTimeSec);
  }
  prefs.putUShort("threshold", moistureThreshold);
  prefs.end();
}
