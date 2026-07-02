#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Board / wiring — defaults per platform
// ---------------------------------------------------------------------------
namespace cfg {
#if defined(ESP8266)
    constexpr int      I2C_SDA       = 4;    // GPIO4 (D2)
    constexpr int      I2C_SCL       = 5;    // GPIO5 (D1)
    constexpr int      RGB_LED       = 2;    // GPIO2 (D4) — built-in LED on many boards
#else
    constexpr int      I2C_SDA       = 8;
    constexpr int      I2C_SCL       = 9;
    constexpr int      RGB_LED       = 48;   // WS2812 on S3 SuperMini clones
#endif

    constexpr uint8_t  MPR121_ADDR   = 0x5A;
    constexpr uint32_t I2C_CLOCK_HZ  = 400000;

    // ---------------------------------------------------------------------------
    // Network
    // ---------------------------------------------------------------------------
    constexpr uint16_t UDP_PORT         = 4242;
    constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
    constexpr uint32_t WIFI_RETRY_INTERVAL_MS  = 2000;

    // ---------------------------------------------------------------------------
    // State machine timings
    // ---------------------------------------------------------------------------
    constexpr uint32_t HELLO_INTERVAL_MS        = 1000;  // beacon cadence in DISCOVERING
    constexpr uint32_t KEEPALIVE_TIMEOUT_MS     = 5000;  // server gone -> back to DISCOVERING
    constexpr uint32_t STREAM_FRAME_INTERVAL_MS = 20;   // ~50 FPS sensor stream
    constexpr uint32_t LED_TICK_INTERVAL_MS     = 50;

    // ---------------------------------------------------------------------------
    // Sensor + framing
    // ---------------------------------------------------------------------------
    constexpr uint8_t  CHANNEL_COUNT = 12;  // all MPR121 electrodes
    constexpr uint32_t TARGET_FPS    = 50;
    constexpr uint32_t FRAME_TIME_MS = 1000 / TARGET_FPS;

    // ---------------------------------------------------------------------------
    // Firmware / protocol IDs
    // ---------------------------------------------------------------------------
    constexpr uint8_t  FW_VERSION = 3;

    // Hand identifiers carried in HELLO
    constexpr uint8_t  HAND_UNKNOWN = 0;
    constexpr uint8_t  HAND_LEFT    = 1;
    constexpr uint8_t  HAND_RIGHT   = 2;
    constexpr uint32_t MAGIC      = 0x534C5031;  // "SLP1"
    constexpr uint8_t  DEVICE_TYPE = 1;          // SloppyHands glove

    // Default MPR121 tuning (kept from v1)
    constexpr uint8_t  MPR_CONFIG1 = 0x10;
    constexpr uint8_t  MPR_CONFIG2 = 0x20;
    constexpr uint8_t  MPR_ECR     = 0x8F;
}