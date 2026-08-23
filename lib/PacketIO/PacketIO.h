#pragma once
#include <Arduino.h>
#include <cstdint>
#include "config.h"

// ---------------------------------------------------------------------------
// Wire protocol. All packets share a 4-byte header for identification.
//
//   Header:  uint32_t magic | uint8_t type | uint8_t fwVersion | uint16_t reserved
//
// Types:
//   1 = HELLO      (device  -> broadcast)   device discovery beacon
//   2 = WELCOME    (server  -> device  )   server accepts device, gives dataPort + keepaliveMs
//   3 = DATA       (device  -> server  )   12-channel sensor frame
//   4 = KEEPALIVE  (server  -> device  )   heartbeat, echoes lastSeenPacketId
//   5 = BYE        (either  -> either  )   graceful close (optional)
// ---------------------------------------------------------------------------

enum class PacketType : uint8_t {
    Hello     = 1,
    Welcome   = 2,
    Data      = 3,
    Keepalive = 4,
    Bye       = 5
};

struct __attribute__((packed)) Header {
    uint32_t magic;
    uint8_t  type;
    uint8_t  fwVersion;
    uint16_t reserved;
};

struct __attribute__((packed)) HelloPacket {
    Header   h;
    uint8_t  mac[6];
    uint8_t  deviceType;
    uint8_t  channelCount;
    uint8_t  hand;       // 0=unknown, 1=left, 2=right
    uint8_t  reserved;
};

struct __attribute__((packed)) WelcomePacket {
    Header   h;
    uint16_t dataPort;
    uint16_t keepaliveMs;
};

struct __attribute__((packed)) DataPacket {
    Header   h;
    uint32_t packetId;
    uint32_t uptimeMs;
    uint16_t filtered[12];     // MPR121 filtered values (zero when inactive)
    uint16_t touchStatus;      // MPR121 touch bitmask
    uint32_t fdcRaw[4];        // FDC2214 28-bit raw (zero when inactive)
    uint8_t  sensorType;       // 0=none, 1=MPR121, 2=FDC2214
    uint8_t  reserved2;
    uint16_t i2cReadTimeMs;
    uint16_t totalLoopTimeMs;
    int8_t   wifiRssi;
    uint8_t  reserved3;
};

struct __attribute__((packed)) KeepalivePacket {
    Header   h;
    uint32_t lastSeenPacketId;
};

namespace PacketIO {
    void initHeader(Header& h, PacketType t);
    void buildHello(HelloPacket& p, const uint8_t mac[6], uint8_t hand);
    void buildData(DataPacket& p, uint32_t packetId,
                   const uint16_t filtered[12], uint16_t touch,
                   const uint32_t fdcRaw[4], uint8_t sensorType,
                   uint16_t i2cMs, uint16_t loopMs, int8_t rssi);

    bool parseWelcome(const uint8_t* buf, size_t len, WelcomePacket& out);
    bool parseKeepalive(const uint8_t* buf, size_t len, KeepalivePacket& out);
    bool isHeaderValid(const Header& h, PacketType expected);
}