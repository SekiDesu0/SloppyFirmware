// Standalone FDC2214 live reader for the Serial Plotter. Does not touch the main firmware.
// Scans the bus once at boot, configures the FDC2214, then streams the 4 raw
// 28-bit channel values as "CH0:x,CH1:y,CH2:z,CH3:w" lines for the Arduino Serial Plotter.

#include <Arduino.h>
#include <Wire.h>

#if defined(ESP8266)
  static const int SDA_PIN = 4;  // GPIO4 (D2)
  static const int SCL_PIN = 5;  // GPIO5 (D1)
#else
  static const int SDA_PIN = 8;
  static const int SCL_PIN = 9;
#endif

// FDC2214 register map (values identical to lib/SensorFDC2214/SensorFDC2214.cpp)
static constexpr uint8_t  REG_DATA_MSB_CH0      = 0x00;  // +2 per channel
static constexpr uint8_t  REG_RCOUNT_CH0        = 0x08;  // +1 per channel
static constexpr uint8_t  REG_OFFSET_CH0        = 0x0C;  // +1 per channel
static constexpr uint8_t  REG_SETTLECOUNT_CH0   = 0x10;  // +1 per channel
static constexpr uint8_t  REG_CLOCK_DIV_CH0     = 0x14;  // +1 per channel
static constexpr uint8_t  REG_ERROR_CONFIG      = 0x19;
static constexpr uint8_t  REG_CONFIG            = 0x1A;
static constexpr uint8_t  REG_MUX_CONFIG        = 0x1B;
static constexpr uint8_t  REG_RESET_DEV         = 0x1C;
static constexpr uint8_t  REG_DRIVE_CURRENT_CH0 = 0x1E;  // +1 per channel
static constexpr uint8_t  REG_MANUFACTURER_ID   = 0x7E;
static constexpr uint8_t  REG_DEVICE_ID         = 0x7F;

static constexpr uint16_t FDC_RCOUNT        = 0x0C00;
static constexpr uint16_t FDC_SETTLECOUNT   = 0x0064;
static constexpr uint16_t FDC_CLOCK_DIV     = 0x2001;
static constexpr uint16_t FDC_DRIVE_CURRENT = 0xF800;
static constexpr uint16_t FDC_MUX_CONFIG    = 0xC20D;
static constexpr uint16_t FDC_CONFIG_ACTIVE = 0x1C01;

static constexpr uint32_t FRAME_TIME_MS = 20;  // ~50 FPS

static uint8_t fdcAddr = 0;

static uint16_t readReg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        Wire.endTransmission();
        return 0xFFFF;
    }
    if (Wire.requestFrom((uint8_t)addr, (uint8_t)2) != 2) return 0xFFFF;
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    return ((uint16_t)hi << 8) | lo;
}

static void writeReg(uint8_t addr, uint8_t reg, uint16_t v) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write((uint8_t)(v >> 8));
    Wire.write((uint8_t)(v & 0xFF));
    Wire.endTransmission();
}

static void fdcInit(uint8_t addr) {
    writeReg(addr, REG_RESET_DEV, 0x8000);  // soft reset
    delay(2);
    for (uint8_t ch = 0; ch < 4; ch++) {
        writeReg(addr, REG_RCOUNT_CH0 + ch,       FDC_RCOUNT);
        writeReg(addr, REG_SETTLECOUNT_CH0 + ch,  FDC_SETTLECOUNT);
        writeReg(addr, REG_OFFSET_CH0 + ch,       0x0000);
        writeReg(addr, REG_CLOCK_DIV_CH0 + ch,    FDC_CLOCK_DIV);
        writeReg(addr, REG_DRIVE_CURRENT_CH0 + ch, FDC_DRIVE_CURRENT);
    }
    writeReg(addr, REG_ERROR_CONFIG, 0x0000);
    writeReg(addr, REG_MUX_CONFIG, FDC_MUX_CONFIG);  // autoscan all 4
    writeReg(addr, REG_CONFIG, FDC_CONFIG_ACTIVE);   // active, internal osc
}

static uint32_t readRaw(uint8_t addr, uint8_t ch) {
    uint8_t base = REG_DATA_MSB_CH0 + ch * 2;
    uint16_t msb = readReg(addr, base);
    uint16_t lsb = readReg(addr, base + 1);
    return ((uint32_t)(msb & 0x0FFF) << 16) | lsb;
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);

    for (int addr = 0x08; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() != 0) continue;
        if (addr == 0x2A || addr == 0x2B) {
            uint16_t mfr = readReg(addr, REG_MANUFACTURER_ID);
            uint16_t dev = readReg(addr, REG_DEVICE_ID);
            Serial.printf("Found 0x%02X  mfr=0x%04X dev=0x%04X%s\r\n",
                          addr, mfr, dev, (dev == 0x3055) ? "  <-- FDC2214" : "");
            if (dev == 0x3055 && !fdcAddr) fdcAddr = addr;
        } else if (addr == 0x5A) {
            Serial.println("Found 0x5A  <-- MPR121");
        } else {
            Serial.printf("Found 0x%02X\r\n", addr);
        }
    }

    if (fdcAddr) {
        fdcInit(fdcAddr);
        delay(5);
        Serial.println("Streaming ch0..ch3 (Teleplot format)");
    } else {
        Serial.println("FDC2214 not found. Check SD->GND, SDA/SCL, VDD.");
    }
}

void loop() {
    if (!fdcAddr) {
        delay(1000);
        return;
    }
    uint32_t raw[4];
    for (uint8_t ch = 0; ch < 4; ch++) raw[ch] = readRaw(fdcAddr, ch);
    Serial.printf(">ch0:%lu\r\n>ch1:%lu\r\n>ch2:%lu\r\n>ch3:%lu\r\n",
                  (unsigned long)raw[0], (unsigned long)raw[1],
                  (unsigned long)raw[2], (unsigned long)raw[3]);
    delay(FRAME_TIME_MS);
}
