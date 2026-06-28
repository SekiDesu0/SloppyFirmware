#pragma once
#include <Arduino.h>

class SensorMPR121 {
public:
    bool begin(uint8_t addr, int sda, int scl, uint32_t clockHz);
    void setSensitivity(uint8_t cfg1, uint8_t cfg2);

    // Read all 12 filtered values into the supplied 12-element array and
    // return the touched-electrodes bitmask from MPR121.
    uint16_t readAll(uint16_t filtered[12]);

    bool present() const { return _present; }

private:
    bool _present = false;
    uint8_t _cfg1 = 0;
    uint8_t _cfg2 = 0;
};