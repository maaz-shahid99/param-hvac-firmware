#ifndef RTC_DS1307_H
#define RTC_DS1307_H

#include <Arduino.h>
#include <RTClib.h>

// I2C bus is shared with BME680 — Wire.begin(6,7) is called in bmeInit()
// so DO NOT call Wire.begin() here again.

struct RTCDateTime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    bool     valid;
    String   timestamp;  // "YYYY-MM-DD HH:MM:SS"
};

void        rtcInit();
bool        rtcIsAvailable();
RTCDateTime rtcGetDateTime();
String      rtcGetTimestamp();  // Returns "YYYY-MM-DD HH:MM:SS" or "BOOT+Xs" fallback
void        rtcSetDateTime(uint16_t year, uint8_t month, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second);

#endif // RTC_DS1307_H