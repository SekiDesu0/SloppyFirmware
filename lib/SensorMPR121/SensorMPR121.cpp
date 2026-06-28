#include "SensorMPR121.h"
#include "Adafruit_MPR121.h"
#include "config.h"

namespace {
    Adafruit_MPR121 cap;
}

bool SensorMPR121::begin(uint8_t addr, int sda, int scl, uint32_t clockHz) {
    Wire.begin(sda, scl);
    Wire.setClock(clockHz);

    if (!cap.begin(addr)) {
        _present = false;
        return false;
    }
    _present = true;
    _cfg1 = cfg::MPR_CONFIG1;
    _cfg2 = cfg::MPR_CONFIG2;

    cap.writeRegister(MPR121_ECR, 0x00);
    cap.writeRegister(MPR121_AUTOCONFIG0, 0x00);
    cap.writeRegister(MPR121_AUTOCONFIG1, 0x00);
    cap.writeRegister(MPR121_CONFIG1, _cfg1);
    cap.writeRegister(MPR121_CONFIG2, _cfg2);
    cap.writeRegister(MPR121_ECR, cfg::MPR_ECR);
    return true;
}

void SensorMPR121::setSensitivity(uint8_t cfg1, uint8_t cfg2) {
    if (!_present) return;
    _cfg1 = cfg1;
    _cfg2 = cfg2;
    cap.writeRegister(MPR121_ECR, 0x00);
    cap.writeRegister(MPR121_CONFIG1, _cfg1);
    cap.writeRegister(MPR121_CONFIG2, _cfg2);
    cap.writeRegister(MPR121_ECR, cfg::MPR_ECR);
}

uint16_t SensorMPR121::readAll(uint16_t filtered[12]) {
    for (uint8_t i = 0; i < cfg::CHANNEL_COUNT; ++i) {
        filtered[i] = cap.filteredData(i);
    }
    return cap.touched();
}