#include "logger.h"

Logger::Logger(uint8_t csPin,
               uint8_t misoPin,
               uint8_t mosiPin,
               uint8_t sckPin) {
  _cs = csPin;
  _miso = misoPin;
  _mosi = mosiPin;
  _sck = sckPin;
}

bool Logger::begin() {
    Serial.println("\n--- SD Logger Init ---");
    
    // Force clean state before init — critical on ESP32C6 after Wire.begin()
    SPI.end();
    delay(200);
    SPI.begin(_sck, _miso, _mosi, _cs);
    delay(200);

    int speeds[] = {1, 2, 4, 8};
    for (int s = 0; s < 4; s++) {
        Serial.printf("[SD] Trying @%dMHz...\n", speeds[s]);
        if (sd.begin(SdSpiConfig(_cs, DEDICATED_SPI, SD_SCK_MHZ(speeds[s])))) {
            Serial.printf("✅ SD Ready @%dMHz\n", speeds[s]);
            _ready = true;
            return true;
        }
        Serial.printf("❌ Failed: 0x%X\n", sd.sdErrorCode());
    }

    Serial.println("❌ SD Init Failed completely.");
    _ready = false;
    return false;
}

bool Logger::isReady() {
  return _ready;
}

void Logger::setFilename(const char* name) {
  _filename = name;
}

void Logger::writeHeader(const String& headerLine) {
  if (!_ready) return;

  if (!sd.exists(_filename)) {
    file = sd.open(_filename, O_WRITE | O_CREAT);
    if (file) {
      file.println(headerLine);
      file.close();
      Serial.println("[SD] Header written.");
    }
  }
}

void Logger::openAppend() {
  // Using the exact flags from your working code
  file = sd.open(_filename, O_WRITE | O_APPEND | O_CREAT);
}

bool Logger::log(const String& line) {
  if (!_ready) return false;

  openAppend();

  if (!file) {
    Serial.println("⚠ SD Open Failed during log attempt");
    return false;
  }

  // 1. Clear any lingering error flags from previous attempts
  file.clearWriteError();

  // 2. Convert Arduino String to a C-string (just like your working code did)
  file.println(line.c_str());

  // 3. Force the ExFat buffer to write to physical flash immediately
  file.sync(); 

  // 4. Check for errors only after the sync
  if (file.getWriteError()) {
    Serial.println("⚠ SD Write Error! (Possible power brownout)");
    file.close();
    return false;
  }

  file.close();
  return true;
}

void Logger::flush() {
  if (file) {
    file.flush();
  }
}