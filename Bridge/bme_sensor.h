#ifndef BME_SENSOR_H
#define BME_SENSOR_H

struct BMEData {
    float temperature;
    float humidity;
    float pressure;
    float gas;
    bool valid;
};

void bmeInit();
bool bmeUpdate();
BMEData bmeGetData();

#endif