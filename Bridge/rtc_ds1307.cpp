#include "rtc_ds1307.h"

static RTC_DS1307 rtc;
static bool _rtcAvailable = false;

void rtcInit() {
    Serial.println("[RTC] Initializing DS1307...");

    // Wire is already started by bmeInit() — just begin the RTC
    if (!rtc.begin()) {
        Serial.println("[RTC] ❌ DS1307 not found. Check wiring.");
        _rtcAvailable = false;
        return;
    }

    if (!rtc.isrunning()) {
        Serial.println("[RTC] ⚠ DS1307 was not running — setting compile time.");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    _rtcAvailable = true;

    DateTime now = rtc.now();
    Serial.printf("[RTC] ✅ DS1307 OK — %04u-%02u-%02u %02u:%02u:%02u\n",
        now.year(), now.month(), now.day(),
        now.hour(), now.minute(), now.second());
}

bool rtcIsAvailable() {
    return _rtcAvailable;
}

RTCDateTime rtcGetDateTime() {
    RTCDateTime result = {0, 0, 0, 0, 0, 0, false, ""};
    if (!_rtcAvailable) return result;

    DateTime now = rtc.now();

    result.year    = now.year();
    result.month   = now.month();
    result.day     = now.day();
    result.hour    = now.hour();
    result.minute  = now.minute();
    result.second  = now.second();
    result.valid   = true;

    char buf[20];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
        result.year, result.month, result.day,
        result.hour, result.minute, result.second);
    result.timestamp = String(buf);

    return result;
}

String rtcGetTimestamp() {
    if (!_rtcAvailable) {
        char buf[20];
        snprintf(buf, sizeof(buf), "BOOT+%lus", millis() / 1000);
        return String(buf);
    }

    RTCDateTime dt = rtcGetDateTime();
    return dt.valid ? dt.timestamp : "INVALID";
}

void rtcSetDateTime(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second) {
    rtc.adjust(DateTime(year, month, day, hour, minute, second));
    Serial.printf("[RTC] DateTime set to %04u-%02u-%02u %02u:%02u:%02u\n",
        year, month, day, hour, minute, second);
}