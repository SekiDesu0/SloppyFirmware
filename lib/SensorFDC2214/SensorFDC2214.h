#pragma once
#include <Arduino.h>

// Minimal register-level driver for the TI FDC2214 4-channel, 28-bit
// capacitance-to-digital converter. No external library dependency.
//
// - Auto-probes the I2C address (ADDR pin selects 0x2A or 0x2B).
// - SD is assumed tied low (chip always active); INTB is not used (polled).
class SensorFDC2214 {
public:
    bool begin(int sda, int scl, uint32_t clockHz);

    // Read all 4 raw 28-bit values into the supplied array.
    void readAll(uint32_t raw[4]);

    bool present() const { return _present; }

private:
    bool     _probe(uint8_t addr);
    uint16_t _readReg(uint8_t reg);
    void     _writeReg(uint8_t reg, uint16_t v);

    uint8_t _addr = 0;
    bool    _present = false;
};
