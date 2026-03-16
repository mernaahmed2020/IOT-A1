#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ESP32Servo.h>

const char* ssid = "Wokwi-GUEST";
const char* password = "";

// Wokwi testing broker
const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;

// ===== Pins =====
#define DHTPIN 4
#define DHTTYPE DHT22
#define PIRPIN 27
#define LDRPIN 34
#define TRIGPIN 5
#define ECHOPIN 18
#define REDLEDPIN 33
#define YELLOWLEDPIN 32
#define BUZZERPIN 25
#define SERVOPIN 14
#define RELAYPIN 26

// ===== Topics =====
const char* TOPIC_TEMP      = "sensors/temperature";
const char* TOPIC_HUM       = "sensors/humidity";
const char* TOPIC_LIGHT     = "sensors/light";
const char* TOPIC_MOTION    = "sensors/motion";
const char* TOPIC_DISTANCE  = "sensors/distance";
const char* TOPIC_STATUS    = "system/status";

const char* TOPIC_LED_CMD      = "actuators/led";
const char* TOPIC_BUZZER_CMD   = "actuators/buzzer";
const char* TOPIC_SERVO_CMD    = "actuators/servo";
const char* TOPIC_RELAY_CMD    = "actuators/relay";
const char* TOPIC_THRESHOLDS   = "config/thresholds";
const char* TOPIC_INTERVAL     = "config/interval";

// ===== Configurable thresholds =====
float tempThreshold = 30.0;
int lightThreshold = 2000;
float distanceThreshold = 15.0;
unsigned long publishInterval = 2500;

// ===== Manual override flags =====
bool manualLed = false;
bool manualBuzzer = false;
bool manualRelay = false;
bool manualServo = false;

bool manualLedState = false;
bool manualBuzzerState = false;
bool manualRelayState = false;
int manualServoAngle = 0;

// ===== Objects =====
WiFiClient espClient;
PubSubClient client(espClient);
DHT dht(DHTPIN, DHTTYPE);
Servo servoMotor;

// ===== State =====
unsigned long lastPublish = 0;
unsigned long lastHeartbeat = 0;
unsigned long buzzerStartTime = 0;
bool buzzerOn = false;
bool lastMotionState = false;

void setupWifi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
}

float readDistanceCM() {
  digitalWrite(TRIGPIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIGPIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIGPIN, LOW);

  long duration = pulseIn(ECHOPIN, HIGH, 30000);
  if (duration == 0) return -1.0;
  return duration * 0.0343 / 2.0;
}

void setBuzzerTimed(bool motion) {
  if (motion && !lastMotionState && !buzzerOn) {
    digitalWrite(BUZZERPIN, HIGH);
    buzzerOn = true;
    buzzerStartTime = millis();
  }

  if (buzzerOn && millis() - buzzerStartTime >= 2000) {
    digitalWrite(BUZZERPIN, LOW);
    buzzerOn = false;
  }

  lastMotionState = motion;
}

String extractValue(String payload, String key) {
  String token = "\"" + key + "\"";
  int keyPos = payload.indexOf(token);
  if (keyPos == -1) return "";

  int colonPos = payload.indexOf(':', keyPos);
  if (colonPos == -1) return "";

  int start = colonPos + 1;
  while (start < payload.length() && (payload[start] == ' ' || payload[start] == '\"')) {
    start++;
  }

  int end = start;
  while (end < payload.length() &&
         payload[end] != ',' &&
         payload[end] != '}' &&
         payload[end] != '\"') {
    end++;
  }

  return payload.substring(start, end);
}

void applyManualCommand(String topic, String payload) {
  if (topic == TOPIC_LED_CMD) {
    String state = extractValue(payload, "state");
    manualLed = true;
    manualLedState = (state == "on" || state == "ON" || state == "true");
  }
  else if (topic == TOPIC_BUZZER_CMD) {
    String state = extractValue(payload, "state");
    manualBuzzer = true;
    manualBuzzerState = (state == "on" || state == "ON" || state == "true");
  }
  else if (topic == TOPIC_SERVO_CMD) {
    String angleStr = extractValue(payload, "angle");
    if (angleStr.length() > 0) {
      manualServo = true;
      manualServoAngle = constrain(angleStr.toInt(), 0, 180);
    }
  }
  else if (topic == TOPIC_RELAY_CMD) {
    String state = extractValue(payload, "state");
    manualRelay = true;
    manualRelayState = (state == "on" || state == "ON" || state == "true");
  }
  else if (topic == TOPIC_THRESHOLDS) {
    String tempStr = extractValue(payload, "temp_max");
    String lightStr = extractValue(payload, "light_min");
    String distStr = extractValue(payload, "dist_min");

    if (tempStr.length() > 0) tempThreshold = tempStr.toFloat();
    if (lightStr.length() > 0) lightThreshold = lightStr.toInt();
    if (distStr.length() > 0) distanceThreshold = distStr.toFloat();

    Serial.println("Thresholds updated from MQTT");
  }
  else if (topic == TOPIC_INTERVAL) {
    String valueStr = extractValue(payload, "value");
    if (valueStr.length() > 0) {
      publishInterval = max(1000UL, (unsigned long)valueStr.toInt());
      Serial.print("Publish interval updated to: ");
      Serial.println(publishInterval);
    }
  }
}

void mqttCallback(char* topic, byte* message, unsigned int length) {
  String payload = "";
  for (unsigned int i = 0; i < length; i++) {
    payload += (char)message[i];
  }

  Serial.print("MQTT received [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(payload);

  applyManualCommand(String(topic), payload);
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP32-SmartEnv-" + String(random(10000));

    if (client.connect(clientId.c_str())) {
      Serial.println(" connected");

      client.subscribe(TOPIC_LED_CMD);
      client.subscribe(TOPIC_BUZZER_CMD);
      client.subscribe(TOPIC_SERVO_CMD);
      client.subscribe(TOPIC_RELAY_CMD);
      client.subscribe(TOPIC_THRESHOLDS);
      client.subscribe(TOPIC_INTERVAL);

      Serial.println("Subscribed to actuator/config topics");
    } else {
      Serial.print(" failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 2 sec");
      delay(2000);
    }
  }
}

void publishHeartbeat() {
  String payload = "{\"uptime\": " + String(millis() / 1000) +
                   ", \"rssi\": " + String(WiFi.RSSI()) + "}";
  client.publish(TOPIC_STATUS, payload.c_str());
  Serial.print("Published status: ");
  Serial.println(payload);
}

void publishSensors() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  bool motion = digitalRead(PIRPIN);
  int light = analogRead(LDRPIN);
  float distance = readDistanceCM();

  bool dhtOk = !(isnan(temperature) || isnan(humidity));

  // ===== Automatic states =====
  bool autoRedLed = dhtOk ? (temperature > tempThreshold) : false;
  bool autoRelay = dhtOk ? (temperature > tempThreshold) : false;
  bool autoYellowLed = light < lightThreshold;
  int autoServoAngle = (distance >= 0 && distance < distanceThreshold) ? 90 : 0;

  // ===== Apply LED =====
  bool finalRedLed = manualLed ? manualLedState : autoRedLed;
  digitalWrite(REDLEDPIN, finalRedLed ? HIGH : LOW);

  // ===== Apply Yellow LED (kept automatic) =====
  digitalWrite(YELLOWLEDPIN, autoYellowLed ? HIGH : LOW);

  // ===== Apply Relay =====
  bool finalRelay = manualRelay ? manualRelayState : autoRelay;
  digitalWrite(RELAYPIN, finalRelay ? HIGH : LOW);

  // ===== Apply Servo =====
  int finalServo = manualServo ? manualServoAngle : autoServoAngle;
  servoMotor.write(finalServo);

  // ===== Apply Buzzer =====
  if (manualBuzzer) {
    digitalWrite(BUZZERPIN, manualBuzzerState ? HIGH : LOW);
    buzzerOn = manualBuzzerState;
  } else {
    setBuzzerTimed(motion);
  }

  // ===== Publish sensor topics =====
  if (dhtOk) {
    String tempPayload = "{\"value\": " + String(temperature, 1) + ", \"unit\": \"°C\"}";
    String humPayload  = "{\"value\": " + String(humidity, 1) + ", \"unit\": \"%\"}";
    client.publish(TOPIC_TEMP, tempPayload.c_str());
    client.publish(TOPIC_HUM, humPayload.c_str());

    Serial.print("Published temperature: ");
    Serial.println(tempPayload);
    Serial.print("Published humidity: ");
    Serial.println(humPayload);
  } else {
    Serial.println("DHT error: TIMEOUT or wiring issue");
  }

  String motionPayload = String("{\"detected\": ") + (motion ? "true" : "false") + "}";
  String lightLevel = autoYellowLed ? "dark" : "bright";
  String lightPayload = "{\"value\": " + String(light) + ", \"level\": \"" + lightLevel + "\"}";

  client.publish(TOPIC_MOTION, motionPayload.c_str());
  client.publish(TOPIC_LIGHT, lightPayload.c_str());

  Serial.print("Published motion: ");
  Serial.println(motionPayload);
  Serial.print("Published light: ");
  Serial.println(lightPayload);

  if (distance >= 0) {
    String distPayload = "{\"value\": " + String(distance, 1) + ", \"unit\": \"cm\"}";
    client.publish(TOPIC_DISTANCE, distPayload.c_str());
    Serial.print("Published distance: ");
    Serial.println(distPayload);
  } else {
    Serial.println("Distance read timeout");
  }

  Serial.print("Red LED: ");
  Serial.println(finalRedLed ? "ON" : "OFF");
  Serial.print("Yellow LED: ");
  Serial.println(autoYellowLed ? "ON" : "OFF");
  Serial.print("Relay: ");
  Serial.println(finalRelay ? "ON" : "OFF");
  Serial.print("Buzzer: ");
  Serial.println(buzzerOn ? "ON" : "OFF");
  Serial.print("Servo: ");
  Serial.print(finalServo);
  Serial.println("°");
  Serial.println("--------------------");
}

void setup() {
  Serial.begin(115200);

  pinMode(PIRPIN, INPUT);
  pinMode(LDRPIN, INPUT);
  pinMode(TRIGPIN, OUTPUT);
  pinMode(ECHOPIN, INPUT);
  pinMode(REDLEDPIN, OUTPUT);
  pinMode(YELLOWLEDPIN, OUTPUT);
  pinMode(BUZZERPIN, OUTPUT);
  pinMode(RELAYPIN, OUTPUT);

  digitalWrite(REDLEDPIN, LOW);
  digitalWrite(YELLOWLEDPIN, LOW);
  digitalWrite(BUZZERPIN, LOW);
  digitalWrite(RELAYPIN, LOW);

  servoMotor.attach(SERVOPIN);
  servoMotor.write(0);

  dht.begin();
  delay(2000);

  setupWifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  Serial.println("ESP32 smart environment system started");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWifi();
  }

  if (!client.connected()) {
    reconnectMQTT();
  }

  client.loop();

  if (!manualBuzzer) {
    setBuzzerTimed(digitalRead(PIRPIN));
  }

  if (millis() - lastPublish >= publishInterval) {
    lastPublish = millis();
    publishSensors();
  }

  if (millis() - lastHeartbeat >= 10000) {
    lastHeartbeat = millis();
    publishHeartbeat();
  }
}