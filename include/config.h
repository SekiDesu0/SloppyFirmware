#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Board / wiring — defaults per platform
// ---------------------------------------------------------------------------
namespace cfg {
#if defined(ESP8266)
    constexpr int      I2C_SDA       = 4;    // GPIO4 (D2)
    constexpr int      I2C_SCL       = 5;    // GPIO5 (D1)
    constexpr int      RGB_LED       = 12;   // GPIO12 (D6) — external NeoPixel; NOT the built-in LED (that's a regular GPIO LED on GPIO2)
    // Battery ADC: ESP8266 has a single input, A0. NodeMCU/D1 Mini boards have
    // an onboard divider mapping 0..3.3 V onto the 0..1 V ADC, so full scale
    // reads as ~3300 mV.
    constexpr int      BATT_ADC_PIN           = A0;
    constexpr uint32_t BATT_ADC_FULL_SCALE_MV = 3300;
#else
    constexpr int      I2C_SDA       = 8;
    constexpr int      I2C_SCL       = 9;
    constexpr int      RGB_LED       = 48;   // WS2812 on S3 SuperMini clones
    // Battery ADC: must be an ADC1 pin (GPIO1..7, 10) - ADC2 cannot be read
    // while the WiFi radio runs, and both transports use WiFi.
    constexpr int      BATT_ADC_PIN           = 4;   // GPIO4 (ADC1_CH3); <0 disables
#endif

    constexpr uint8_t  MPR121_ADDR   = 0x5A;
    constexpr uint32_t I2C_CLOCK_HZ  = 400000;

    // FDC2214: I2C address is set by the ADDR pin (L=0x2A, H=0x2B). The driver
    // auto-probes both. SD is tied low on the board (chip always active); INTB
    // is left unconnected (data is polled over I2C).
    constexpr uint8_t  FDC2214_ADDR_0      = 0x2A;
    constexpr uint8_t  FDC2214_ADDR_1      = 0x2B;
    constexpr uint8_t  FDC_CHANNEL_COUNT   = 4;      // CH0..CH3, 28-bit raw
    // Per-channel conversion timing. RCOUNT trades resolution vs speed:
    //   RCOUNT=0xFFFF -> max resolution, ~26ms/channel (~9 Hz across 4 ch)
    //   RCOUNT=0x0C00 -> ~3ms/channel, ~12-bit resolution (~50 Hz across 4 ch)
    // SETTLECOUNT = settle cycles before sampling; higher lets each channel's
    // LC tank ring up fully so the previous channel doesn't bleed in (TI EVM
    // uses 0x0064). Cost is negligible at our RCOUNT (~5 ms full sweep).
    constexpr uint16_t FDC_RCOUNT          = 0x0C00;
    constexpr uint16_t FDC_SETTLECOUNT     = 0x0064;

    // ---------------------------------------------------------------------------
    // Network
    // ---------------------------------------------------------------------------
    constexpr uint16_t UDP_PORT         = 4242;
    constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
    constexpr uint32_t WIFI_RETRY_INTERVAL_MS  = 2000;

    // ESP-NOW link (trackers <-> dongle). Tracker hops channels while DISCOVERING
    // until a WELCOME arrives; the dongle listens on its fixed channel.
    constexpr uint8_t  ESPNOW_CHANNEL_DEFAULT  = 1;
    constexpr uint8_t  ESPNOW_CHANNEL_MIN      = 1;
    constexpr uint8_t  ESPNOW_CHANNEL_MAX      = 13;   // EU/US overlap: use <=11 for US-only setups
    constexpr uint32_t ESPNOW_HOP_INTERVAL_MS  = 300;  // per-channel dwell in DISCOVERING
    constexpr uint8_t  ESPNOW_MAX_PEERS        = 8;    // dongle peer table size

    // ---------------------------------------------------------------------------
    // State machine timings
    // ---------------------------------------------------------------------------
    constexpr uint32_t HELLO_INTERVAL_MS        = 1000;  // beacon cadence in DISCOVERING
    constexpr uint32_t KEEPALIVE_TIMEOUT_MS     = 5000;  // server gone -> back to DISCOVERING
    constexpr uint32_t STREAM_FRAME_INTERVAL_MS = 20;   // ~50 FPS sensor stream
    constexpr uint32_t LED_TICK_INTERVAL_MS     = 50;

    // ---------------------------------------------------------------------------
    // Battery monitor
    //
    // ADC across a resistor divider from VBAT:
    //     VBAT --[R_TOP]--+--[R_BOT]-- GND
    //                     |
    //                  ADC pin
    // vbat = tap_mv * (R_TOP + R_BOT) / R_BOT. Percent maps EMPTY..FULL linearly.
    // Set BATT_ADC_PIN < 0 to disable (frames report 0 mV / percent 255).
    // ---------------------------------------------------------------------------
    constexpr float    BATT_DIVIDER_R_TOP = 100.0f;   // kOhm, VBAT side
    constexpr float    BATT_DIVIDER_R_BOT = 100.0f;   // kOhm, GND side
    constexpr float    BATT_EMPTY_VOLTS   = 3.30f;    // 0 %
    constexpr float    BATT_FULL_VOLTS    = 4.20f;    // 100 %
    constexpr uint8_t  BATT_PCT_UNKNOWN   = 255;

    // ---------------------------------------------------------------------------
    // Sensor + framing
    // ---------------------------------------------------------------------------
    constexpr uint8_t  CHANNEL_COUNT = 12;  // all MPR121 electrodes
    constexpr uint32_t TARGET_FPS    = 50;
    constexpr uint32_t FRAME_TIME_MS = 1000 / TARGET_FPS;

    // ---------------------------------------------------------------------------
    // Firmware / protocol IDs
    // ---------------------------------------------------------------------------
    constexpr uint8_t  FW_VERSION = 6;

    // Hand identifiers carried in HELLO
    constexpr uint8_t  HAND_UNKNOWN = 0;
    constexpr uint8_t  HAND_LEFT    = 1;
    constexpr uint8_t  HAND_RIGHT   = 2;

    // Runtime transport selection (persisted in NVS/EEPROM). `espnow` streams to
    // an ESP32-S3 HID dongle over ESP-NOW; `wifi` uses the classic UDP path.
    constexpr uint8_t  TRANSPORT_WIFI    = 0;
    constexpr uint8_t  TRANSPORT_ESPNOW  = 1;
    constexpr uint8_t  TRANSPORT_DEFAULT = TRANSPORT_ESPNOW;

    // Sensor type carried in DATA.sensorType
    constexpr uint8_t  SENSOR_TYPE_NONE    = 0;
    constexpr uint8_t  SENSOR_TYPE_MPR121  = 1;
    constexpr uint8_t  SENSOR_TYPE_FDC2214 = 2;

    // Runtime sensor selection (persisted in NVS/EEPROM). `auto` probes FDC2214
    // first, then falls back to MPR121. Override with `sensor mpr121|fdc2214`.
    constexpr uint8_t  SENSOR_MODE_AUTO    = 0;
    constexpr uint8_t  SENSOR_MODE_MPR121  = 1;
    constexpr uint8_t  SENSOR_MODE_FDC2214 = 2;
    constexpr uint8_t  SENSOR_MODE_DEFAULT = SENSOR_MODE_AUTO;
    constexpr uint32_t MAGIC      = 0x534C5031;  // "SLP1"
    constexpr uint8_t  DEVICE_TYPE = 1;          // SloppyHands tracker

    // Default MPR121 tuning (kept from v1)
    constexpr uint8_t  MPR_CONFIG1 = 0x10;
    constexpr uint8_t  MPR_CONFIG2 = 0x20;
    constexpr uint8_t  MPR_ECR     = 0x8F;
}