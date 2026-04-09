N/*
 * XIAO ESP32C3 Temp Monitor - SdFat FINAL (Compiles Clean!)
 */

#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <Wire.h>
#include <RTClib.h>
#include <SdFat.h>
#include <SPI.h>

#define DHT_TYPE DHT22
#define ONE_WIRE_BUS 2
#define DHT_PIN 5
#define I2C_SDA 6
#define I2C_SCL 7
#define SD_CS 3
#define SD_SCK 8
#define SD_MOSI 10
#define SD_MISO 4  // ← Change to 0 or 1 if GPIO4 is used

SdExFat sd;
ExFile logFile;                      // ← File32 for SdFat Adafruit Fork
const char* logFileName = "/log.csv";
bool sdCardAvailable = false;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DHT dht(DHT_PIN, DHT_TYPE);
RTC_DS3231 rtc;
DeviceAddress sensor1Address, sensor2Address;
int numberOfDevices;
bool rtcAvailable = false;
unsigned long lastSampleTime = 0;
#define SAMPLE_INTERVAL 2000

// ─── SETUP ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== XIAO ESP32C3 SdFat FINAL ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  initializeRTC();
  initializeSensors();
  initializeDHT();
  initializeSdFat();

  Serial.println("\n=== READY! ===\n");
}

// ─── LOOP ────────────────────────────────────────────────
void loop() {
  if (millis() - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime = millis();
    readAndLog();
  }
}

// ─── RTC ─────────────────────────────────────────────────
void initializeRTC() {
  Serial.println("--- RTC ---");
  if (rtc.begin()) {
    rtcAvailable = true;
    if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println("✓ RTC OK  Time: " + rtc.now().timestamp());
  } else {
    Serial.println("⚠ No RTC - using millis()");
  }
}

// ─── DS18B20 ─────────────────────────────────────────────
void initializeSensors() {
  Serial.println("--- DS18B20 ---");
  sensors.begin();
  numberOfDevices = sensors.getDeviceCount();
  Serial.print("✓ Found: "); Serial.println(numberOfDevices);
  if (numberOfDevices >= 1) {
    sensors.getAddress(sensor1Address, 0);
    sensors.setResolution(sensor1Address, 12);
  }
  if (numberOfDevices >= 2) {
    sensors.getAddress(sensor2Address, 1);
    sensors.setResolution(sensor2Address, 12);
  }
}

// ─── DHT22 ───────────────────────────────────────────────
void initializeDHT() {
  Serial.println("--- DHT22 ---");
  dht.begin();
  delay(2000);
  float t = dht.readTemperature();
  Serial.println(isnan(t) ? "⚠ DHT22 warn" : "✓ DHT22 OK");
}

// ─── SD CARD (FIXED cardSize) ─────────────────────────────
// 

void initializeSdFat() {
  Serial.println("--- SdFat Diagnostic ---");

  // Test all pin combos + speeds
  int sckPins[]  = {8, 10};
  int mosiPins[] = {10, 8};
  int speeds[]   = {1, 2, 4, 8};

  for (int p = 0; p < 2; p++) {
    for (int s = 0; s < 4; s++) {
      SPI.end();
      delay(100);
      SPI.begin(sckPins[p], SD_MISO, mosiPins[p], SD_CS);
      delay(100);

      Serial.printf("Trying SCK=%d MOSI=%d @%dMHz...\n",
                    sckPins[p], mosiPins[p], speeds[s]);

      if (sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI,
                               SD_SCK_MHZ(speeds[s])))) {
        Serial.printf("✅ SUCCESS! SCK=%d MOSI=%d @%dMHz\n",
                      sckPins[p], mosiPins[p], speeds[s]);
        sdCardAvailable = true;

        if (!sd.exists(logFileName)) {
          logFile = sd.open(logFileName, O_WRITE | O_CREAT);
          if (logFile) {
            // Replace old header line with:
            logFile.println("DateTime,DS1_C,DS1_F,DS2_C,DS2_F,DHT_C,DHT_F,DHT_H%,RTC_C,RTC_F");
            logFile.close();
          }
        }
        Serial.println("Log ready");
        return;  // Stop trying once found!
      }
      Serial.printf("❌ Failed: 0x%X\n", sd.sdErrorCode());
      SPI.end();
      delay(200);
    }
  }

  Serial.println("❌ ALL combinations failed!");
  Serial.println("Check wiring or try different SD card");
}


// ─── READ & LOG ───────────────────────────────────────────
void readAndLog() {
  sensors.requestTemperatures();

  // DS18B20 - Celsius
  float ds1C = numberOfDevices >= 1 ? sensors.getTempC(sensor1Address) : -127;
  float ds2C = numberOfDevices >= 2 ? sensors.getTempC(sensor2Address) : -127;
  
  // DS18B20 - Fahrenheit (convert or read directly)
  float ds1F = numberOfDevices >= 1 ? sensors.getTempF(sensor1Address) : -196.6;
  float ds2F = numberOfDevices >= 2 ? sensors.getTempF(sensor2Address) : -196.6;

  // DHT22
  float dhtC = dht.readTemperature();        // Celsius
  float dhtF = dht.readTemperature(true);    // Fahrenheit (pass true!)
  float dhtH = dht.readHumidity();

  // RTC
  float rtcC = rtcAvailable ? rtc.getTemperature() : 0;
  float rtcF = (rtcC * 9.0 / 5.0) + 32.0;  // Convert

  String timeStr = rtcAvailable ? rtc.now().timestamp() : String(millis() / 1000) + "s";

  // Serial display
  Serial.println("=====");
  Serial.printf("Time : %s\n",         timeStr.c_str());
  Serial.printf("DS1  : %.2f°C / %.2f°F\n", ds1C, ds1F);
  Serial.printf("DS2  : %.2f°C / %.2f°F\n", ds2C, ds2F);
  Serial.printf("DHT  : %.2f°C / %.2f°F  Hum: %.1f%%\n", dhtC, dhtF, dhtH);
  Serial.printf("RTC  : %.2f°C / %.2f°F\n", rtcC, rtcF);

  // SD log
  if (sdCardAvailable) {
    logFile = sd.open(logFileName, O_WRITE | O_APPEND | O_CREAT);
    if (logFile) {
      logFile.printf("%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f\n",
        timeStr.c_str(),
        ds1C, ds1F,
        ds2C, ds2F,
        dhtC, dhtF,
        dhtH,
        rtcC, rtcF);
      logFile.close();
      Serial.println("Logged ✓");
    } else {
      Serial.println("Card Open failed");
      sdCardAvailable = false;
    }
  }
  Serial.println("=====\n");
}


