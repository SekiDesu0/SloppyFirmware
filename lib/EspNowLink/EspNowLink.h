#pragma once
#include <Arduino.h>
#include "config.h"

// Cross-platform (ESP8266 + ESP32/ESP32-S3) ESP-NOW link layer.
//
// Both ends run WIFI_STA on a fixed Wi-Fi channel. Frames are delivered to
// poll() from loop(): RX callbacks fire in the WiFi stack context, so they
// only push into an internal ring buffer here.
//
// Used by the trackers (via EspNowTransport) and by the HID dongle directly.

struct RxFrame {
    uint8_t  mac[6];        // sender MAC
    int8_t   rssi;          // rx RSSI (0 = unknown, e.g. ESP8266)
    uint16_t len;
    uint8_t  data[250];     // ESP-NOW max payload
};

class EspNowLink {
public:
    static constexpr uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // Brings up STA mode + ESP-NOW on `channel`. Safe to call again to change
    // channel. Returns false if the radio stack refused.
    bool begin(uint8_t channel);

    // Switch channel, re-registering all added peers on it.
    bool setChannel(uint8_t channel);
    uint8_t channel() const { return _channel; }

    // Register/unregister a unicast peer (broadcast peer is implicit).
    bool addPeer(const uint8_t mac[6]);
    void removePeer(const uint8_t mac[6]);

    // Send a frame. `mac == nullptr` broadcasts. Returns true if accepted by
    // the radio stack (delivery itself is fire-and-forget).
    bool send(const uint8_t* mac, const uint8_t* buf, size_t len);

    // Drain one received frame. Call repeatedly until it returns false.
    bool poll(RxFrame& out);

    uint32_t drops() const { return _drops; }   // frames lost to full ring

    // Platform callback surface: RX callbacks fire outside any method, so
    // they need access. Do not call these yourself.
    static EspNowLink* _self;
    void pushRx(const uint8_t* mac, int8_t rssi, const uint8_t* data, int len);

private:
    void _readdPeers();                         // platform peer bookkeeping

    static constexpr int RING_SLOTS = 8;        // power of two
    RxFrame _ring[RING_SLOTS];
    volatile uint8_t _head = 0;                 // producer (rx cb)
    volatile uint8_t _tail = 0;                 // consumer (poll)

    uint8_t  _peers[cfg::ESPNOW_MAX_PEERS][6];
    uint8_t  _peerCount = 0;
    uint8_t  _channel   = cfg::ESPNOW_CHANNEL_DEFAULT;
    uint32_t _drops     = 0;
    bool     _started   = false;
};
