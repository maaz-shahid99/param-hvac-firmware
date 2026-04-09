#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <SdFat.h>
#include <SPI.h>

class Logger {
public:
    Logger(uint8_t csPin,
           uint8_t misoPin,
           uint8_t mosiPin,
           uint8_t sckPin);

    bool begin();
    bool isReady();

    void setFilename(const char* name);
    void writeHeader(const String& headerLine);
    bool log(const String& line);
    void flush();

private:
    SdExFat sd;
    ExFile file;

    uint8_t _cs, _miso, _mosi, _sck;
    const char* _filename = "/log.csv";
    bool _ready = false;

    void openAppend();
};

#endif