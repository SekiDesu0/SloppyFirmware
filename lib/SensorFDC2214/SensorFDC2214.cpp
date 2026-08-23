#include "SensorFDC2214.h"
#include <Wire.h>
#include "config.h"

// FDC2214 register map (16-bit, big-endian over I2C).
namespace {
    constexpr uint8_t REG_DATA_MSB_CH0     = 0x00;  // +2 per channel (MSB/LSB pairs)
    constexpr uint8_t REG_RCOUNT_CH0       = 0x08;  // +1 per channel
    constexpr uint8_t REG_OFFSET_CH0       = 0x0C;  // +1 per channel
    constexpr uint8_t REG_SETTLECOUNT_CH0  = 0x10;  // +1 per channel
    constexpr uint8_t REG_CLOCK_DIV_CH0    = 0x14;  // +1 per channel
    constexpr uint8_t REG_STATUS           = 0x18;
    constexpr uint8_t REG_ERROR_CONFIG     = 0x19;
    constexpr uint8_t REG_CONFIG           = 0x1A;
    constexpr uint8_t REG_MUX_CONFIG       = 0x1B;
    constexpr uint8_t REG_RESET_DEV        = 0x1C;
    constexpr uint8_t REG_DRIVE_CURRENT_CH0 = 0x1E; // +1 per channel
    constexpr uint8_t REG_MANUFACTURER_ID  = 0x7E;
    constexpr uint8_t REG_DEVICE_ID        = 0x7F;

    constexpr uint16_t MANUFACTURER_ID     = 0x5449;  // "TI"
    constexpr uint16_t DEVICE_ID           = 0x3055;  // FDC2214

    // CLOCK_DIVIDERS: CH_FIN_SEL high-frequency path (sensor < 10 MHz -> /2) + FREF divider 1.
    constexpr uint16_t CLOCK_DIVIDERS      = 0x2001;
    // DRIVE_CURRENT: IDRIVE = 0x1F (~1.5 mA, max), INIT_IDRIVE = 0.
    constexpr uint16_t DRIVE_CURRENT       = 0xF800;
    // MUX_CONFIG: autoscan enabled | RR sequence all channels | reserved (0x0208) | deglitch 10 MHz (0x0005).
    constexpr uint16_t MUX_CONFIG          = 0xC20D;
    // CONFIG: reserved (bits 12,10,0 = 0x1401) | SENSOR_ACTIVATE_SEL (low-power) | active CH0 | not sleeping.
    constexpr uint16_t CONFIG_ACTIVE       = 0x1C01;
}

bool SensorFDC2214::begin(int sda, int scl, uint32_t clockHz) {
    Wire.begin(sda, scl);
    Wire.setClock(clockHz);

    _present = false;
    _addr = 0;
    const uint8_t addrs[] = {cfg::FDC2214_ADDR_0, cfg::FDC2214_ADDR_1};
    for (uint8_t a : addrs) {
        if (_probe(a)) {
            _addr = a;
            _present = true;
            break;
        }
    }
    if (!_present) return false;

    // Soft reset, then configure all 4 channels and start conversions.
    _writeReg(REG_RESET_DEV, 0x8000);
    delay(2);

    for (uint8_t i = 0; i < cfg::FDC_CHANNEL_COUNT; ++i) {
        _writeReg(REG_RCOUNT_CH0 + i,      cfg::FDC_RCOUNT);
        _writeReg(REG_SETTLECOUNT_CH0 + i, cfg::FDC_SETTLECOUNT);
        _writeReg(REG_OFFSET_CH0 + i,      0x0000);
        _writeReg(REG_CLOCK_DIV_CH0 + i,   CLOCK_DIVIDERS);
        _writeReg(REG_DRIVE_CURRENT_CH0 + i, DRIVE_CURRENT);
    }
    _writeReg(REG_ERROR_CONFIG, 0x0000);
    _writeReg(REG_MUX_CONFIG, MUX_CONFIG);
    _writeReg(REG_CONFIG, CONFIG_ACTIVE);
    return true;
}

void SensorFDC2214::readAll(uint32_t raw[4]) {
    for (uint8_t i = 0; i < cfg::FDC_CHANNEL_COUNT; ++i) {
        uint8_t base = REG_DATA_MSB_CH0 + i * 2;
        uint16_t msb = _readReg(base);
        uint16_t lsb = _readReg(base + 1);
        // Top 4 bits of DATA_MSB are error flags; data is 28 bits.
        raw[i] = ((uint32_t)(msb & 0x0FFF) << 16) | lsb;
    }
}

bool SensorFDC2214::_probe(uint8_t addr) {
    _addr = addr;
    return _readReg(REG_DEVICE_ID) == DEVICE_ID &&
           _readReg(REG_MANUFACTURER_ID) == MANUFACTURER_ID;
}

uint16_t SensorFDC2214::_readReg(uint8_t reg) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        Wire.endTransmission();
        return 0;
    }
    if (Wire.requestFrom(_addr, (uint8_t)2) != 2) return 0;
    uint16_t hi = Wire.read();
    uint16_t lo = Wire.read();
    return (hi << 8) | lo;
}

void SensorFDC2214::_writeReg(uint8_t reg, uint16_t v) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write((uint8_t)(v >> 8));
    Wire.write((uint8_t)(v & 0xFF));
    Wire.endTransmission();
}
