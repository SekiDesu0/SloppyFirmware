#include "BatteryMonitor.h"
#include "config.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#endif

// Raw calibrated reading at the divider tap, in mV.
uint16_t BatteryMonitor::_sampleTapMv() const {
#if defined(ESP32)
    // Factory-calibrated conversion (11 dB attenuation set in begin()).
    return (uint16_t)analogReadMilliVolts(_pin);
#else
    // ESP8266: 10-bit ADC; NodeMCU/D1 Mini onboard divider maps ~3.3 V full
    // scale (see BATT_ADC_FULL_SCALE_MV in config.h).
    return (uint16_t)((uint32_t)analogRead(_pin) * cfg::BATT_ADC_FULL_SCALE_MV / 1023);
#endif
}

void BatteryMonitor::begin() {
    if (cfg::BATT_ADC_PIN < 0) {
        _enabled = false;
        _mv = 0;
        return;
    }
    _pin = cfg::BATT_ADC_PIN;
#if defined(ESP32)
    analogSetAttenuation(ADC_11db);     // widest input range (~0..3.1 V)
#endif
    _enabled = true;
}

uint8_t BatteryMonitor::read() {
    if (!_enabled) return cfg::BATT_PCT_UNKNOWN;

    float tapMv = (float)_sampleTapMv();
    float cellMv = tapMv * (cfg::BATT_DIVIDER_R_TOP + cfg::BATT_DIVIDER_R_BOT)
                         / cfg::BATT_DIVIDER_R_BOT;

    // EMA with alpha ~0.04 -> ~1 s time constant at 50 FPS. Prime on first use.
    _mv = _mv ? (uint16_t)(_mv * 0.96f + cellMv * 0.04f) : (uint16_t)(cellMv + 0.5f);

    const float span = cfg::BATT_FULL_VOLTS - cfg::BATT_EMPTY_VOLTS;
    if (span <= 0.0f) return cfg::BATT_PCT_UNKNOWN;
    float pct = (_mv / 1000.0f - cfg::BATT_EMPTY_VOLTS) / span * 100.0f;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)(pct + 0.5f);
}
