#pragma once
#include <Arduino.h>

// Battery gauge via an external resistor divider on an ADC pin. All knobs
// (pin, divider resistors, empty/full voltage) live in include/config.h.
//
// read() is called once per streamed frame (~50 Hz); an EMA smooths the raw
// tap reading so the percent display doesn't jitter. Disabled (BATT_ADC_PIN
// < 0) reports percent 255 (= unknown) and 0 mV.
class BatteryMonitor {
public:
    void begin();

    // Sample the ADC, smooth, convert. Returns percent 0..100, or
    // cfg::BATT_PCT_UNKNOWN when disabled.
    uint8_t read();

    // Smoothed cell voltage in mV (0 = unknown/disabled).
    uint16_t milliVolts() const { return _mv; }

    bool enabled() const { return _enabled; }

private:
    uint16_t _sampleTapMv() const;

    bool     _enabled = false;
    int      _pin     = -1;
    uint16_t _mv      = 0;   // smoothed cell voltage, mV
};
