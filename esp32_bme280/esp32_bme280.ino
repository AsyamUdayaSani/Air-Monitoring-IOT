#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <time.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <ArduinoJson.h> // NEW: install "ArduinoJson" by Benoit Blanchon via Library Manager

// ---------------------------------------------------------
// 1. WIFI & MQTT CONFIGURATION
// ---------------------------------------------------------
const char* ssid = "ANRI";
const char* password = "";
const char* mqtt_server = "10.1.0.251";
const int mqtt_port = 1883;
const char* mqtt_topic = "arsip/sensor_bme280";
const char* mqtt_config_topic = "arsip/sensor_bme280/config";
const char* mqtt_cmd_topic = "arsip/sensor_bme280/cmd"; // NEW: listens for remote offset changes

WiFiClient espClient;
PubSubClient client(espClient);

// ---------------------------------------------------------
// 2. SENSOR & CALIBRATION CONFIGURATION
// ---------------------------------------------------------
#define I2C_SDA 21
#define I2C_SCL 22
Adafruit_BME280 bme;

LiquidCrystal_I2C lcd(0x27, 16, 2);

const float TEMP_MULTIPLIER = 1.07;
float TEMP_OFFSET = 0.0;
const float HUM_MULTIPLIER = 0.812;
float HUM_OFFSET = -2.0;

const float DEFAULT_TEMP_OFFSET = 0.0;
const float DEFAULT_HUM_OFFSET = 0.0;

const float TEMP_OFFSET_STEP = 0.1;
const float HUM_OFFSET_STEP = 1.0;

unsigned long lastUpdate = 0;
unsigned long lastMqttAttempt = 0;
const long updateInterval = 3000;

const long sensorReadInterval = 30000; // 30 seconds

bool showOffsets = false;
bool offsetsDirty = false;

// ---------------------------------------------------------
// SCREEN CYCLING (Temp/Hum <-> Pressure), NORMAL mode only
// ---------------------------------------------------------
enum NormalScreen { SCREEN_TEMP_HUM, SCREEN_PRESSURE };
NormalScreen currentScreen = SCREEN_TEMP_HUM;
unsigned long lastScreenSwitch = 0;
const long screenCycleInterval = 4000; // switch screens every 4 seconds

// ---------------------------------------------------------
// PERSISTENT STORAGE (survives reboots/power loss)
// ---------------------------------------------------------
Preferences prefs;
unsigned long rebootCount = 0;

// ---------------------------------------------------------
// SCHEDULED FULL REBOOT
// ---------------------------------------------------------
unsigned long lastFullReboot = 0;
const unsigned long fullRebootInterval = 43200000UL; // 12 hours

// ---------------------------------------------------------
// 3. NTP CONFIGURATION (WIB = UTC+7)
// ---------------------------------------------------------
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

// ---------------------------------------------------------
// 4. PUSH BUTTON PIN CONFIGURATION (4 buttons)
// ---------------------------------------------------------
#define BTN_MODE  25
#define BTN_UP    33
#define BTN_DOWN  32
#define BTN_RESET 35

struct Button {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  unsigned long lastDebounceTime;
  unsigned long lastAcceptedTime;
};

Button btnMode  = {BTN_MODE,  HIGH, HIGH, 0, 0};
Button btnUp    = {BTN_UP,    HIGH, HIGH, 0, 0};
Button btnDown  = {BTN_DOWN,  HIGH, HIGH, 0, 0};
Button btnReset = {BTN_RESET, HIGH, HIGH, 0, 0};

const unsigned long debounceDelay = 50;
const unsigned long pressCooldown = 250;

bool buttonPressed(Button &b) {
  bool reading = digitalRead(b.pin);

  if (reading != b.lastReading) {
    b.lastDebounceTime = millis();
  }

  bool pressedEvent = false;
  if ((millis() - b.lastDebounceTime) > debounceDelay) {
    if (reading != b.stableState) {
      b.stableState = reading;
      if (b.stableState == LOW) {
        if (millis() - b.lastAcceptedTime > pressCooldown) {
          pressedEvent = true;
          b.lastAcceptedTime = millis();
        }
      }
    }
  }

  b.lastReading = reading;
  return pressedEvent;
}

// ---------------------------------------------------------
// 5. MODE STATE MACHINE & UI ASSETS
// ---------------------------------------------------------
enum Mode { NORMAL, EDIT_TEMP, EDIT_HUM };
Mode currentMode = NORMAL;

int lastDirection = 0;

unsigned long lastFlashToggle = 0;
bool flashVisible = true;
const long flashInterval = 400;

byte upArrow[8] = {
  0b00100, 0b01110, 0b10101, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000
};
byte downArrow[8] = {
  0b00100, 0b00100, 0b00100, 0b00100, 0b10101, 0b01110, 0b00100, 0b00000
};

byte tempCLogo[8] = {
  0b01100,
  0b10010,
  0b01100,
  0b00000,
  0b00110,
  0b01000,
  0b01000,
  0b00110
};

// ---------------------------------------------------------
// HELPER: GET CURRENT TIME
// ---------------------------------------------------------
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00";
  }
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf);
}

// ---------------------------------------------------------
// HELPER: FORMAT FLOAT WITH COMMA DECIMAL SEPARATOR (for LCD display only)
// ---------------------------------------------------------
String formatComma(float value, int decimals) {
  String s = String(value, decimals);
  s.replace('.', ',');
  return s;
}

// ---------------------------------------------------------
// HELPER: SAVE OFFSETS TO FLASH
// ---------------------------------------------------------
void saveOffsetsToFlash() {
  prefs.begin("calib", false);
  prefs.putFloat("tempOffset", TEMP_OFFSET);
  prefs.putFloat("humOffset", HUM_OFFSET);
  prefs.end();
}

// ---------------------------------------------------------
// HELPER: LOAD OFFSETS FROM FLASH
// ---------------------------------------------------------
void loadOffsetsFromFlash() {
  prefs.begin("calib", true);
  TEMP_OFFSET = prefs.getFloat("tempOffset", DEFAULT_TEMP_OFFSET);
  HUM_OFFSET = prefs.getFloat("humOffset", DEFAULT_HUM_OFFSET);
  prefs.end();

  Serial.print("Loaded saved offsets -> Temp: ");
  Serial.print(TEMP_OFFSET);
  Serial.print(", Hum: ");
  Serial.println(HUM_OFFSET);
}

// ---------------------------------------------------------
// HELPER: READ SENSOR (Forced Mode)
// ---------------------------------------------------------
bool readSensor(float &tempOut, float &humOut, float &presOut) {
  if (!bme.takeForcedMeasurement()) {
    return false;
  }

  float raw_temp = bme.readTemperature();
  float raw_hum = bme.readHumidity();
  float raw_pres = bme.readPressure() / 100.0F;

  if (isnan(raw_temp) || isnan(raw_hum)) {
    return false;
  }

  tempOut = (raw_temp * TEMP_MULTIPLIER) + TEMP_OFFSET;
  humOut = (raw_hum * HUM_MULTIPLIER) + HUM_OFFSET;
  presOut = raw_pres;
  return true;
}

// ---------------------------------------------------------
// HELPER: PUBLISH OFFSETS
// ---------------------------------------------------------
void publishOffsets() {
  if (!client.connected()) return;

  String ts = getTimestamp();
  String payload = "{";
  payload += "\"temp_offset\":" + String(TEMP_OFFSET, 2) + ",";
  payload += "\"hum_offset\":" + String(HUM_OFFSET, 2) + ",";
  payload += "\"timestamp\":\"" + ts + "\"";
  payload += "}";

  Serial.print("Publishing config: ");
  Serial.println(payload);
  client.publish(mqtt_config_topic, payload.c_str(), true);
  offsetsDirty = false;
}

// ---------------------------------------------------------
// NEW: MQTT CALLBACK - handles incoming remote commands
// ---------------------------------------------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.println("Failed to parse incoming command JSON");
    return;
  }

  if (doc.containsKey("temp_offset")) {
    TEMP_OFFSET = doc["temp_offset"].as<float>();
    Serial.print("Remote command: TEMP_OFFSET set to ");
    Serial.println(TEMP_OFFSET);
  }
  if (doc.containsKey("hum_offset")) {
    HUM_OFFSET = doc["hum_offset"].as<float>();
    Serial.print("Remote command: HUM_OFFSET set to ");
    Serial.println(HUM_OFFSET);
  }

  saveOffsetsToFlash();
  offsetsDirty = true; // triggers a republish of the confirmed offset on next loop
}

// ---------------------------------------------------------
// HELPER: PRINT SIGNED OFFSET
// ---------------------------------------------------------
void printSignedOffset(float value, int decimals) {
  if (value >= 0) {
    lcd.print("+");
  }
  lcd.print(formatComma(value, decimals));
}

// ---------------------------------------------------------
// DISPLAY: NORMAL MODE - TEMP/HUM SCREEN
// ---------------------------------------------------------
void showTempHumScreen(float temp, float hum) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(formatComma(temp, 1));
  lcd.write(byte(2));

  lcd.setCursor(0, 1);
  lcd.print("H:");
  lcd.print((int)round(hum));
  lcd.print("%");

  if (showOffsets) {
    lcd.setCursor(9, 0);
    lcd.print("T");
    printSignedOffset(TEMP_OFFSET, 1);

    lcd.setCursor(9, 1);
    lcd.print("H");
    printSignedOffset(HUM_OFFSET, 0);
  } else {
    lcd.setCursor(9, 0);
    lcd.print("1:A 2:M");
    lcd.setCursor(9, 1);
    lcd.print("3:R 4:D");
  }
}

// ---------------------------------------------------------
// DISPLAY: NORMAL MODE - PRESSURE SCREEN
// ---------------------------------------------------------
void showPressureScreen(float pres) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Tekanan Udara:");

  lcd.setCursor(0, 1);
  lcd.print(formatComma(pres, 1));
  lcd.print(" hPa");
}

// ---------------------------------------------------------
// DISPLAY: NORMAL MODE (picks the active screen)
// ---------------------------------------------------------
void showNormalDisplay(float temp, float hum, float pres) {
  if (currentScreen == SCREEN_TEMP_HUM) {
    showTempHumScreen(temp, hum);
  } else {
    showPressureScreen(pres);
  }
}

// ---------------------------------------------------------
// DISPLAY: EDIT MODE
// ---------------------------------------------------------
void showEditDisplay(float temp, float hum) {
  if (millis() - lastFlashToggle > flashInterval) {
    lastFlashToggle = millis();
    flashVisible = !flashVisible;

    lcd.clear();

    if (currentMode == EDIT_TEMP) {
      lcd.setCursor(0, 1);
      lcd.print("H:");
      lcd.print((int)round(hum));
      lcd.print("%");

      if (flashVisible) {
        lcd.setCursor(0, 0);
        lcd.print("T:");
        lcd.print(formatComma(temp, 1));
        lcd.write(byte(2));

        lcd.setCursor(13, 0);
        if (lastDirection == 1) {
          lcd.write(byte(0));
        } else if (lastDirection == -1) {
          lcd.write(byte(1));
        }
      }
    } else if (currentMode == EDIT_HUM) {
      lcd.setCursor(0, 0);
      lcd.print("T:");
      lcd.print(formatComma(temp, 1));
      lcd.write(byte(2));

      if (flashVisible) {
        lcd.setCursor(0, 1);
        lcd.print("H:");
        lcd.print((int)round(hum));
        lcd.print("%");

        lcd.setCursor(13, 1);
        if (lastDirection == 1) {
          lcd.write(byte(0));
        } else if (lastDirection == -1) {
          lcd.write(byte(1));
        }
      }
    }
  }
}

// ---------------------------------------------------------
// HELPER: CONNECT TO MQTT
// ---------------------------------------------------------
void reconnect() {
  Serial.print("Attempting MQTT connection...");
  String clientId = "ESP32_ArchiveRoom-";
  clientId += String(random(0xffff), HEX);

  if (client.connect(clientId.c_str())) {
    Serial.println("connected!");
    client.subscribe(mqtt_cmd_topic); // NEW: listen for remote offset commands
    publishOffsets();
  } else {
    Serial.print("failed, rc=");
    Serial.println(client.state());
  }
}

// ---------------------------------------------------------
// HARDWARE CHECKER (I2C SCANNER)
// ---------------------------------------------------------
void runI2CScanner() {
  byte error, address;
  int nDevices = 0;
  Serial.println("\n--- RUNNING ESP32 HARDWARE CHECKER ---");
  Serial.println("Scanning I2C Bus (SDA:21, SCL:22) for devices...");

  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("Device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);

      if (address == 0x27 || address == 0x3F) Serial.println("   -> (This is likely your LCD Screen)");
      if (address == 0x76 || address == 0x77) Serial.println("   -> (This is likely your BME280 Sensor)");

      nDevices++;
    }
  }

  if (nDevices == 0) {
    Serial.println("No devices found! Check your wiring (3.3V, GND, SDA, SCL).");
  } else {
    Serial.print("Hardware check complete. Found ");
    Serial.print(nDevices);
    Serial.println(" device(s).");
  }
  Serial.println("--------------------------------------\n");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_RESET, INPUT);

  Wire.begin(I2C_SDA, I2C_SCL);

  runI2CScanner();
  delay(1000);

  lcd.init();
  lcd.backlight();
  lcd.createChar(0, upArrow);
  lcd.createChar(1, downArrow);
  lcd.createChar(2, tempCLogo);

  lcd.setCursor(0, 0);
  lcd.print("Memulai Sensor..");

  if (!bme.begin(0x76, &Wire)) {
    Serial.println("Error: Could not find a valid BME280 sensor!");
    lcd.clear();
    lcd.print("BME280 Error!");
    while (1) delay(10);
  }

  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::FILTER_OFF);

  loadOffsetsFromFlash();

  prefs.begin("system", false);
  rebootCount = prefs.getULong("rebootCount", 0);
  rebootCount++;
  prefs.putULong("rebootCount", rebootCount);
  prefs.end();

  Serial.print("Total reboots so far (persisted log): ");
  Serial.println(rebootCount);

  lcd.setCursor(0, 1);
  lcd.print("Sensor OK!");
  delay(1000);

  float temp, hum, pres;
  readSensor(temp, hum, pres);
  showNormalDisplay(temp, hum, pres);
  delay(1500);

  Serial.println("\n--- NETWORK IDENTITY CHECK ---");
  WiFi.mode(WIFI_STA);
  Serial.print("ESP32 MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP32 IP (Offline): ");
  Serial.println(WiFi.localIP());
  Serial.println("------------------------------\n");

  Serial.println("Starting WiFi connection in background...");
  WiFi.begin(ssid, password);

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback); // NEW: register the command handler

  lastFullReboot = millis();
  lastScreenSwitch = millis();
}

void loop() {
  unsigned long now = millis();

  // ---------------------------------------------------------
  // SCHEDULED FULL REBOOT (every 12 hours)
  // ---------------------------------------------------------
  if (now - lastFullReboot > fullRebootInterval) {
    Serial.println("\n*** Scheduled 12-hour reboot triggered ***\n");
    Serial.print("This will be reboot #");
    Serial.println(rebootCount + 1);

    if (client.connected()) {
      client.disconnect();
    }
    delay(200);
    ESP.restart();
  }

  bool forceUpdate = false;

  if (buttonPressed(btnMode)) {
    if (currentMode == NORMAL) {
      currentMode = EDIT_TEMP;
    } else if (currentMode == EDIT_TEMP) {
      currentMode = EDIT_HUM;
    } else {
      currentMode = NORMAL;
    }
    lastDirection = 0;
    flashVisible = true;
    lastFlashToggle = now;
    showOffsets = false;
    forceUpdate = true;
  }

  if (buttonPressed(btnReset)) {
    if (currentMode == NORMAL) {
      showOffsets = !showOffsets;
      lastUpdate = 0;
    } else if (currentMode == EDIT_TEMP) {
      TEMP_OFFSET = DEFAULT_TEMP_OFFSET;
      lastDirection = 0;
      offsetsDirty = true;
      forceUpdate = true;
    } else if (currentMode == EDIT_HUM) {
      HUM_OFFSET = DEFAULT_HUM_OFFSET;
      lastDirection = 0;
      offsetsDirty = true;
      forceUpdate = true;
    }
  }

  if (buttonPressed(btnUp)) {
    if (currentMode == EDIT_TEMP) {
      TEMP_OFFSET += TEMP_OFFSET_STEP;
      lastDirection = 1;
      offsetsDirty = true;
      forceUpdate = true;
    } else if (currentMode == EDIT_HUM) {
      HUM_OFFSET += HUM_OFFSET_STEP;
      lastDirection = 1;
      offsetsDirty = true;
      forceUpdate = true;
    }
  }

  if (buttonPressed(btnDown)) {
    if (currentMode == EDIT_TEMP) {
      TEMP_OFFSET -= TEMP_OFFSET_STEP;
      lastDirection = -1;
      offsetsDirty = true;
      forceUpdate = true;
    } else if (currentMode == EDIT_HUM) {
      HUM_OFFSET -= HUM_OFFSET_STEP;
      lastDirection = -1;
      offsetsDirty = true;
      forceUpdate = true;
    }
  }

  if (offsetsDirty && client.connected()) {
    publishOffsets();
    saveOffsetsToFlash();
  }

  static float temp = 0.0, hum = 0.0, pres = 0.0;
  static bool validReading = true;
  static unsigned long lastSensorRead = 0;

  if (now - lastSensorRead > sensorReadInterval || lastSensorRead == 0 || forceUpdate) {
    lastSensorRead = now;
    validReading = readSensor(temp, hum, pres);

    if (forceUpdate && currentMode != NORMAL) {
      lastFlashToggle = 0;
    }
  }

  // ---------------------------------------------------------
  // AUTO-CYCLE BETWEEN TEMP/HUM AND PRESSURE SCREENS (NORMAL mode only)
  // ---------------------------------------------------------
  if (currentMode == NORMAL && now - lastScreenSwitch > screenCycleInterval) {
    lastScreenSwitch = now;
    currentScreen = (currentScreen == SCREEN_TEMP_HUM) ? SCREEN_PRESSURE : SCREEN_TEMP_HUM;
    lastUpdate = 0; // force an immediate redraw so the switch feels instant
  }

  if (!validReading) {
    if (now - lastUpdate > updateInterval) {
      lcd.clear();
      lcd.print("Sensor Error!");
      lastUpdate = now;
    }
  } else if (currentMode == NORMAL) {
    if (now - lastUpdate > updateInterval || forceUpdate) {
      lastUpdate = now;
      showNormalDisplay(temp, hum, pres);

      if (!forceUpdate && client.connected()) {
        String ts = getTimestamp();
        String payload = "{";
        payload += "\"temperature\":" + String(temp, 2) + ",";
        payload += "\"humidity\":" + String(hum, 2) + ",";
        payload += "\"pressure\":" + String(pres, 2) + ",";
        payload += "\"timestamp\":\"" + ts + "\"";
        payload += "}";

        Serial.print("Publishing sensor: ");
        Serial.println(payload);
        client.publish(mqtt_topic, payload.c_str());
      }
    }
  } else {
    showEditDisplay(temp, hum);
  }

  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (isConnected && !wasConnected) {
    Serial.println("\n=================================");
    Serial.println("WiFi Connected!");
    Serial.print("ESP32 IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("=================================\n");
  }
  wasConnected = isConnected;

  if (isConnected) {
    if (!client.connected()) {
      if (now - lastMqttAttempt > 5000) {
        lastMqttAttempt = now;
        reconnect();
      }
    } else {
      client.loop();
    }
  }
}