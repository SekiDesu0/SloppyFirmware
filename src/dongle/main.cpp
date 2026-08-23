// SloppyFirmware v5 - ESP32-S3 HID dongle
//
// Plugs into the PC over USB: vendor-defined HID input reports carrying
// TUNNEL frames (DATA payload + source MAC/RSSI) from paired gloves.
//
// Radio side: listens for ESP-NOW HELLO broadcasts from gloves, answers with
// WELCOME, then streams KEEPALIVEs and forwards their DATA frames to the PC.
// One fixed Wi-Fi channel (persisted in NVS); gloves hop channels to find us.
//
// No CDC console: reflash via BOOT+RST to change configuration.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Adafruit_TinyUSB.h>

#include "config.h"
#include "PacketIO.h"
#include "EspNowLink.h"
#include "StatusLED.h"

#ifndef DONGLE_RGB_LED
#define DONGLE_RGB_LED 48          // DevKitC-1 v1.0 / SuperMini clones WS2812
#endif

#ifndef DONGLE_KEEPALIVE_MS
#define DONGLE_KEEPALIVE_MS 1000
#endif

#ifndef DONGLE_PEER_TIMEOUT_MS
#define DONGLE_PEER_TIMEOUT_MS 5000
#endif

// --- USB HID -----------------------------------------------------------------
// Full-speed interrupt endpoints cap at 64 bytes/transaction, so each 82-byte
// TUNNEL frame ships as two input reports:
//   Report ID 1 : first 64 bytes of TunnelPacket
//   Report ID 2 : remaining sizeof(TunnelPacket)-64 bytes
static constexpr uint16_t HID_REPORT_LEN = sizeof(TunnelPacket);
// Each wire chunk = 1 report-id byte + payload must fit the 64-byte EP buffer.
static constexpr uint8_t  HID_PART1_LEN  = 62;
static constexpr uint8_t  HID_PART2_LEN  = HID_REPORT_LEN - HID_PART1_LEN;

static const uint8_t hidReportDescriptor[] = {
    0x06, 0x00, 0xFF,               // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,                     // Usage (0x01)
    0xA1, 0x01,                     // Collection (Application)

    0x85, 0x01,                     //   Report ID (1)
    0x09, 0x01,                     //   Usage (0x01)
    0x15, 0x00,                     //   Logical Minimum (0)
    0x26, 0xFF, 0x00,               //   Logical Maximum (255)
    0x75, 0x08,                     //   Report Size (8 bits)
    0x95, HID_PART1_LEN,            //   Report Count (64)
    0x81, 0x02,                     //   Input (Data, Variable, Absolute)

    0x85, 0x02,                     //   Report ID (2)
    0x09, 0x01,                     //   Usage (0x01)
    0x15, 0x00,                     //   Logical Minimum (0)
    0x26, 0xFF, 0x00,               //   Logical Maximum (255)
    0x75, 0x08,                     //   Report Size (8 bits)
    0x95, HID_PART2_LEN,            //   Report Count (18)
    0x81, 0x02,                     //   Input (Data, Variable, Absolute)

    0xC0                            // End Collection
};

Adafruit_USBD_HID usbHid;

// --- Globals -------------------------------------------------------------------
EspNowLink nowLink;
StatusLED  led;

struct Peer {
    uint8_t  mac[6];
    uint32_t lastSeenMs;
    uint32_t lastPacketId;
    bool     active;
};

Peer       peers[cfg::ESPNOW_MAX_PEERS];
uint8_t    peerCount   = 0;
uint32_t   forwardedPkts = 0;
uint32_t   helloReplies  = 0;
uint8_t    chan          = cfg::ESPNOW_CHANNEL_DEFAULT;

// --- Peer table ------------------------------------------------------------------
Peer* findPeer(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < peerCount; i++) {
        if (memcmp(peers[i].mac, mac, 6) == 0) return &peers[i];
    }
    return nullptr;
}

Peer* upsertPeer(const uint8_t mac[6]) {
    Peer* p = findPeer(mac);
    if (!p) {
        if (peerCount >= cfg::ESPNOW_MAX_PEERS) return nullptr;
        p = &peers[peerCount++];
        memset(p, 0, sizeof(Peer));
        memcpy(p->mac, mac, 6);
    }
    return p;
}

bool anyActivePeer() {
    for (uint8_t i = 0; i < peerCount; i++) {
        if (peers[i].active) return true;
    }
    return false;
}

uint8_t countActivePeers() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < peerCount; i++) {
        if (peers[i].active) n++;
    }
    return n;
}

// --- ESP-NOW rx --------------------------------------------------------------------
static void handleFrame(const RxFrame& f) {
    if (f.len < (int)sizeof(Header)) return;
    Header h;
    memcpy(&h, f.data, sizeof(h));
    if (h.magic != cfg::MAGIC) return;

    switch (h.type) {
        case static_cast<uint8_t>(PacketType::Hello): {
            if (f.len < (int)sizeof(HelloPacket)) break;
            Peer* p = upsertPeer(f.mac);
            if (p) p->lastSeenMs = millis();
            nowLink.addPeer(f.mac);                 // ensure unicast path exists
            // WELCOME back: dataPort unused on ESP-NOW; keepalive cadence only.
            WelcomePacket w;
            PacketIO::initHeader(w.h, PacketType::Welcome);
            w.dataPort    = 0;
            w.keepaliveMs = DONGLE_KEEPALIVE_MS;
            nowLink.send(f.mac, (const uint8_t*)&w, sizeof(w));
            helloReplies++;
            break;
        }

        case static_cast<uint8_t>(PacketType::Data): {
            if (f.len != (int)sizeof(DataPacket)) break;
            Peer* p = upsertPeer(f.mac);
            if (!p) break;
            DataPacket d;
            memcpy(&d, f.data, sizeof(d));
            p->lastSeenMs   = millis();
            p->lastPacketId = d.packetId;
            p->active       = true;
            TunnelPacket t;
            PacketIO::buildTunnel(t, f.mac, f.rssi, d);
            // Split across two report IDs (62 + 20 bytes). TinyUSB keeps one
            // pending report per instance, so the pair must be atomic: if the
            // first chunk queues, spin until the second one takes (the EP
            // drains within ~one poll interval). If chunk 1 is busy, drop the
            // whole frame so halves can never mis-pair.
            const uint8_t* tp = reinterpret_cast<const uint8_t*>(&t);
            if (usbHid.sendReport(1, tp, HID_PART1_LEN)) {
                uint8_t guard = 0;
                while (!usbHid.sendReport(2, tp + HID_PART1_LEN, HID_PART2_LEN)) {
                    delayMicroseconds(50);
                    if (++guard > 200) break;   // ~10 ms worst case
                }
            }
            forwardedPkts++;
            break;
        }

        case static_cast<uint8_t>(PacketType::Bye): {
            Peer* p = findPeer(f.mac);
            if (p) p->active = false;
            break;
        }

        default:
            break;      // WELCOME/KEEPALIVE/TUNNEL are not for the dongle
    }
}

// --- Setup / loop ----------------------------------------------------------------------
void setup() {
    // Serial maps to UART0 (CDC disabled): optional debug escape hatch on pins
    // TXD0/RXD0. Writes are buffered and never block the radio loop.
    Serial.begin(115200);

    led.begin(DONGLE_RGB_LED);
    led.setState(DeviceState::Discovering);

    Serial.printf("[BOOT] SloppyHands dongle FW%u\r\n", cfg::FW_VERSION);

    Preferences prefs;
    prefs.begin("dongle", true);
    chan = prefs.getUChar("ch", cfg::ESPNOW_CHANNEL_DEFAULT);
    if (chan < cfg::ESPNOW_CHANNEL_MIN || chan > cfg::ESPNOW_CHANNEL_MAX)
        chan = cfg::ESPNOW_CHANNEL_DEFAULT;
    prefs.end();

    // TinyUSB device: HID only (single interface).
    // detach -> configure -> attach forces a clean re-enumeration with our
    // descriptors and VID/PID (the prebuilt core ignores ARDUINO_USB_PID).
    TinyUSBDevice.detach();
    delay(50);
    TinyUSBDevice.setManufacturerDescriptor("SloppyHands");
    TinyUSBDevice.setProductDescriptor("SloppyHands ESP-NOW Dongle");
    TinyUSBDevice.setSerialDescriptor("SLP-DONGLE-1");
    TinyUSBDevice.setID(0x303A, 0x534C);
    usbHid.setPollInterval(2);
    usbHid.setReportDescriptor(hidReportDescriptor, sizeof(hidReportDescriptor));
    usbHid.begin();
    TinyUSBDevice.attach();

    if (!nowLink.begin(chan)) {
        Serial.println("[BOOT] FATAL: ESP-NOW init failed.");
        led.setState(DeviceState::Connecting);      // solid red = radio dead
    } else {
        Serial.printf("[BOOT] Dongle listening on ESP-NOW ch%u\r\n", chan);
    }
}

void loop() {
    led.tick();

    // Drain radio
    RxFrame f;
    while (nowLink.poll(f)) handleFrame(f);

    // Keepalives + expiry sweep
    static uint32_t lastKaMs = 0;
    static bool     ledWasStreaming = false;
    uint32_t now = millis();
    if (now - lastKaMs >= DONGLE_KEEPALIVE_MS) {
        lastKaMs = now;
        KeepalivePacket k;
        PacketIO::initHeader(k.h, PacketType::Keepalive);
        for (uint8_t i = 0; i < peerCount; i++) {
            if (!peers[i].active) continue;
            k.lastSeenPacketId = peers[i].lastPacketId;
            nowLink.send(peers[i].mac, (const uint8_t*)&k, sizeof(k));
        }
    }
    for (uint8_t i = 0; i < peerCount; i++) {
        if (peers[i].active && now - peers[i].lastSeenMs > DONGLE_PEER_TIMEOUT_MS) {
            peers[i].active = false;
        }
    }
    bool isActive = anyActivePeer();
    if (isActive != ledWasStreaming) {
        ledWasStreaming = isActive;
        led.setState(isActive ? DeviceState::Streaming : DeviceState::Discovering);
    }

    static uint32_t lastHbMs = 0;
    if (now - lastHbMs >= 5000) {
        lastHbMs = now;
        Serial.printf("[HB] ch=%u peers=%u active=%u fwd=%lu\r\n",
                      chan, peerCount, countActivePeers(),
                      (unsigned long)forwardedPkts);
    }

    delay(1);
}
