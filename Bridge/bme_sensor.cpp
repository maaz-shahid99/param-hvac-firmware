#include "bme_sensor.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

#define SDA_PIN 6
#define SCL_PIN 7
#define BME_ADDRESS 0x77
#define BME_INTERVAL_MS 5000

Adafruit_BME680 bme;
static unsigned long lastReadTime = 0;

static BMEData currentData = {0, 0, 0, 0, false};

void bmeInit() {
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);

    if (!bme.begin(BME_ADDRESS, &Wire)) {
        Serial.println("[BME] Sensor not found!");
        currentData.valid = false;
        return;
    }

    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);

    Serial.println("[BME] Initialized successfully");
}

bool bmeUpdate() {

    if (millis() - lastReadTime < BME_INTERVAL_MS)
        return false;

    lastReadTime = millis();

    if (!bme.performReading()) {
        Serial.println("[BME] Reading failed");
        currentData.valid = false;
        return false;
    }

    currentData.temperature = bme.temperature;
    currentData.humidity    = bme.humidity;
    currentData.pressure    = bme.pressure / 100.0;
    currentData.gas         = bme.gas_resistance / 1000.0;
    currentData.valid       = true;

    // One concise line instead of a 6-line block (full data still logged to storage).
    Serial.printf("[BME] %.1fC  %.1f%%RH  %.1fhPa  gas %.0fk\n",
                  currentData.temperature, currentData.humidity,
                  currentData.pressure, currentData.gas);

    return true;
}

BMEData bmeGetData() {
    return currentData;
}