// SloppyFirmware v5 - ESP32-S3 HID dongle
//
// Plugs into the PC over USB: vendor-defined HID input reports carrying
// TUNNEL frames (DATA payload + source MAC/RSSI) from paired trackers.
//
// Radio side: listens for ESP-NOW HELLO broadcasts from trackers, answers with
// WELCOME, then streams KEEPALIVEs and forwards their DATA frames to the PC.
// One fixed Wi-Fi channel (persisted in NVS); trackers hop channels to find us.
//
// USB side uses the Arduino core's native TinyUSB stack (USBHID classes) - no
// external TinyUSB library, so two copies of the stack can never fight over
// the PHY. ARDUINO_USB_CDC_ON_BOOT must stay 0 (see platformio.ini): the
// core's pre-setup auto-start breaks enumeration otherwise. The composite
// device is HID (tracker data) + CDC (live console); UART0 mirrors the logs.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "USB.h"
#include "USBHID.h"
#include "class/cdc/cdc_device.h"   // direct tud_cdc_n_* output (DTR-independent)
#include "device/usbd.h"            // tud_mounted()

#include "config.h"
#include "PacketIO.h"
#include "EspNowLink.h"
#include "StatusLED.h"

#if ARDUINO_USB_MODE || !SOC_USB_OTG_SUPPORTED || !CONFIG_TINYUSB_HID_ENABLED
#error "Dongle needs a native-USB build: ARDUINO_USB_MODE=0 with core TinyUSB HID enabled (see platformio.ini)"
#endif

#ifndef DONGLE_RGB_LED
#define DONGLE_RGB_LED 48          // DevKitC-1 v1.0 / SuperMini clones WS2812
#endif

#ifndef DONGLE_KEEPALIVE_MS
#define DONGLE_KEEPALIVE_MS 1000
#endif

#ifndef DONGLE_PEER_TIMEOUT_MS
#define DONGLE_PEER_TIMEOUT_MS 5000
#endif

// Run CPU at 80 MHz instead of 240: non-heatsink builds run hot, and the
// workload (forwarding ~8 KB/s of tracker frames to USB HID) barely needs
// cycles. TinyUSB's 48 MHz PHY clock is independent of the CPU frequency.
static constexpr unsigned long CPU_FREQ_MHZ = 80;

// --- USB HID -----------------------------------------------------------------
// Full-speed interrupt endpoints cap at 64 bytes/transaction, so each 85-byte
// TUNNEL frame ships as two input reports:
//   Report ID 1 : first 62 bytes of TunnelPacket
//   Report ID 2 : remaining sizeof(TunnelPacket)-62 bytes
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
    0x95, HID_PART1_LEN,            //   Report Count (62)
    0x81, 0x02,                     //   Input (Data, Variable, Absolute)

    0x85, 0x02,                     //   Report ID (2)
    0x09, 0x01,                     //   Usage (0x01)
    0x15, 0x00,                     //   Logical Minimum (0)
    0x26, 0xFF, 0x00,               //   Logical Maximum (255)
    0x75, 0x08,                     //   Report Size (8 bits)
    0x95, HID_PART2_LEN,            //   Report Count (23)
    0x81, 0x02,                     //   Input (Data, Variable, Absolute)

    0xC0                            // End Collection
};

// Vendor-defined HID device on the core's TinyUSB stack: one interface,
// input only. SendReport() blocks on a completion semaphore (100 ms timeout),
// so each report is fully drained before the next call returns.
class TunnelHID : public USBHIDDevice {
public:
    void begin() {
        USBHID::addDevice(this, sizeof(hidReportDescriptor));
        _hid.begin();
    }
    bool sendPart(uint8_t id, const void* data, size_t len) {
        return _hid.SendReport(id, data, len);
    }

private:
    uint16_t _onGetDescriptor(uint8_t* buffer) override {
        memcpy(buffer, hidReportDescriptor, sizeof(hidReportDescriptor));
        return sizeof(hidReportDescriptor);
    }
    USBHID _hid;
};

TunnelHID tunnelHid;

// The core's ready-made "USBSerial" global only exists when
// ARDUINO_USB_CDC_ON_BOOT=1, which we must keep off (its pre-setup auto-start
// breaks enumeration), so build our own CDC port on interface 0 instead.
static USBCDC dongleCdc(0);

// --- Globals -------------------------------------------------------------------
EspNowLink nowLink;
StatusLED  led;

struct Peer {
    uint8_t  mac[6];
    uint32_t lastSeenMs;
    uint32_t lastPacketId;
    int8_t   lastRssi;
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
            if (p) {
                p->lastSeenMs = millis();
                p->lastRssi   = f.rssi;
            }
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
            p->lastRssi     = f.rssi;
            p->active       = true;
            TunnelPacket t;
            PacketIO::buildTunnel(t, f.mac, f.rssi, d);
            // Ship as two input reports (62 + 23 bytes). SendReport() blocks
            // until the host has taken the previous report, so back-to-back
            // calls cannot interleave: if part 1 fails to queue within its
            // timeout, part 2 never goes out and the whole frame is dropped
            // rather than mis-paired.
            const uint8_t* tp = reinterpret_cast<const uint8_t*>(&t);
            if (tunnelHid.sendPart(1, tp, HID_PART1_LEN)) {
                tunnelHid.sendPart(2, tp + HID_PART1_LEN, HID_PART2_LEN);
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

// --- Console -------------------------------------------------------------------
// Logs go to UART0 (bench escape hatch) and to the CDC interface of the USB
// composite device, so a terminal on the dongle's ttyACM port sees live
// output while it streams.
//
// CDC output bypasses USBCDC::write(): the core's wrapper silently drops
// everything unless the host asserts DTR, but terminals like the tracker
// console keep DTR low. Direct TinyUSB calls deliver as soon as the device
// is configured; once the 64 B endpoint FIFO fills, extra bytes drop
// (non-blocking), which is fine for log/console traffic.
static void cdcOut(const char* s, size_t n) {
    if (!n || !tud_mounted()) return;
    // Push through the 64 B endpoint FIFO, yielding while the host drains it,
    // so bursts longer than one USB frame don't drop bytes. Worst case block
    // is bounded (~50 ms) and only ever hit while printing long CLI output.
    // NOTE: hosts that keep DTR asserted get bit-exact output; hosts holding
    // DTR low (rare - the tracker console raises DTR after opening) may still
    // lose small chunks inside bursts longer than ~2 USB frames. Short
    // outputs (status lines, prompts, echoes) are unaffected.
    size_t off = 0;
    uint32_t spins = 0;
    while (off < n && spins < 500) {
        size_t w = tud_cdc_n_write(0, s + off, n - off);
        tud_cdc_n_write_flush(0);
        if (w) {
            off += w;
            spins = 0;
        } else {
            delayMicroseconds(100);
            spins++;
        }
    }
}

static void cliPrint(const char* s) {
    cdcOut(s, strlen(s));
}

static void cliPrintln(const char* s) {
    cdcOut(s, strlen(s));
    cdcOut("\r\n", 2);
}

static void cliPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void cliPrintf(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) cdcOut(buf, n);
}

static void logf(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    Serial.write(buf, n);
    cdcOut(buf, n);
}

// --- Console CLI -----------------------------------------------------------------
// Lives on the USB CDC interface only: the dongle hangs off a single USB
// cable, so UART0 stays a logs-only escape hatch for bench debugging. Same
// conventions as the tracker CLI: local echo, backspace/DEL editing, '> '
// prompt. All output goes through cdcOut(), so it works even when the host
// keeps DTR low; the banner prints on DTR rise or on the first keystroke.
static String cliLine;
static bool   cliPrompted = false;
static bool   cliLastWasCR = false;

static void cliHelp() {
    cliPrintln("Commands:");
    cliPrintln("  status       FW / channel / peer summary");
    cliPrintln("  list         Known tracker peers (age, RSSI, last packet)");
    cliPrintln("  channel <n>  Set ESP-NOW Wi-Fi channel, persist & reboot");
    cliPrintln("  forget       Drop the in-RAM peer table (trackers re-pair)");
    cliPrintln("  reset        Soft-reset the dongle");
    cliPrintln("  help         Show this message");
}

static void cliStatus() {
    cliPrintf("fw=%u ch=%u\r\n", cfg::FW_VERSION, chan);
    cliPrintf("peers: known=%u active=%u forwarded=%lu helloReplies=%lu\r\n",
                     peerCount, countActivePeers(),
                     (unsigned long)forwardedPkts, (unsigned long)helloReplies);
    cliPrintf("usb: mounted=%d uptime=%lus\r\n",
                     (int)(bool)USB, millis() / 1000);
}

static void cliList() {
    if (!peerCount) {
        cliPrintln("Peer table empty.");
        return;
    }
    uint32_t now = millis();
    for (uint8_t i = 0; i < peerCount; i++) {
        const Peer& p = peers[i];
        cliPrintf("%02X:%02X:%02X:%02X:%02X:%02X %s age=%lus rssi=%d pktId=%lu\r\n",
                         p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
                         p.active ? "active" : "stale ",
                         (unsigned long)((now - p.lastSeenMs) / 1000),
                         (int)p.lastRssi, (unsigned long)p.lastPacketId);
    }
}

static void cliExec(const String& line) {
    String t = line;
    t.trim();
    if (!t.length()) return;

    if (t == "help")   { cliHelp();   return; }
    if (t == "status") { cliStatus(); return; }
    if (t == "list")   { cliList();   return; }

    if (t.startsWith("channel ")) {
        int n = t.substring(8).toInt();
        if (n < cfg::ESPNOW_CHANNEL_MIN || n > cfg::ESPNOW_CHANNEL_MAX) {
            cliPrintf("Usage: channel <%u..%u>\r\n",
                             cfg::ESPNOW_CHANNEL_MIN, cfg::ESPNOW_CHANNEL_MAX);
            return;
        }
        Preferences prefs;
        prefs.begin("dongle", false);
        prefs.putUChar("ch", (uint8_t)n);
        prefs.end();
        cliPrintf("Channel %d stored. Rebooting...\r\n", n);
        delay(100);
        ESP.restart();
        return;
    }

    if (t == "forget") {
        peerCount = 0;
        memset(peers, 0, sizeof(peers));
        cliPrintln("Peer table dropped. Trackers will re-pair.");
        return;
    }

    if (t == "reset") {
        cliPrintln("Rebooting...");
        delay(100);
        ESP.restart();
        return;
    }

    cliPrintln("Unknown command. Type 'help'.");
}

static void cliPump() {
    // Reads are pumped regardless of DTR (hosts may keep it low). The banner
    // prints when DTR rises or, failing that, on the first keystroke.
    static bool lastDtr = false;
    bool dtr = (bool)dongleCdc;
    if (!dtr) lastDtr = false;

    if (!cliPrompted && (dtr || dongleCdc.available())) {
        cliPrompted = true;
        cliLine = "";
        cdcOut("\r\n", 2);
        cliPrintf("SloppyHands dongle FW%u - console ready.\r\n", cfg::FW_VERSION);
        cliHelp();
        cliPrint("> ");
    }
    if (dtr && !lastDtr && cliPrompted) {
        // Re-show the prompt alone when a terminal re-attaches mid-session.
        cliPrint("\r\n> ");
    }
    lastDtr = dtr;

    while (dongleCdc.available()) {
        char c = (char)dongleCdc.read();
        if (c == '\n' && cliLastWasCR) {          // swallow LF of a CRLF pair
            cliLastWasCR = false;
            continue;
        }
        cliLastWasCR = (c == '\r');
        if (c == '\r' || c == '\n') {
            cliPrint("\r\n");
            cliExec(cliLine);
            cliLine = "";
            cliPrint("> ");
        } else if (c == 0x08 || c == 0x7F) {          // backspace / DEL
            if (cliLine.length()) {
                cliLine.remove(cliLine.length() - 1);
                cdcOut("\b \b", 3);
            }
        } else if (c >= 0x20 && c < 0x7F) {           // printable: local echo
            if (cliLine.length() < 128) {
                cliLine += c;
                { char cc = c; cdcOut(&cc, 1); }
            }
        }
    }
}

// --- Setup / loop ----------------------------------------------------------------------
void setup() {
    // Serial maps to UART0 (CDC_ON_BOOT=0); dongleCdc joins the USB composite
    // as a second console over the same cable.
    Serial.begin(115200);

    setCpuFrequencyMhz(CPU_FREQ_MHZ);

    led.begin(DONGLE_RGB_LED);
    led.setState(DeviceState::Discovering);

    logf("[BOOT] SloppyHands dongle FW%u cpu=%luMHz\r\n",
         cfg::FW_VERSION, (unsigned long)getCpuFrequencyMhz());

    Preferences prefs;
    prefs.begin("dongle", true);
    chan = prefs.getUChar("ch", cfg::ESPNOW_CHANNEL_DEFAULT);
    if (chan < cfg::ESPNOW_CHANNEL_MIN || chan > cfg::ESPNOW_CHANNEL_MAX)
        chan = cfg::ESPNOW_CHANNEL_DEFAULT;
    prefs.end();

    // Native-USB composite: vendor-defined HID + CDC console. Descriptor
    // strings/IDs must be set before USB.begin() enumerates the device; with
    // CDC_ON_BOOT=0 this is the only USB start (no pre-setup auto-begin from
    // the core).
    USB.manufacturerName("SloppyHands");
    USB.productName("SloppyHands ESP-NOW Dongle");
    USB.serialNumber("SLP-DONGLE-1");
    USB.VID(0x303A);                       // Espressif vendor space; PID "SL" -
    USB.PID(0x534C);                       // matched by test_tracker.py
    tunnelHid.begin();
    dongleCdc.begin(115200);
    // Keep CDC reboot-on-esptool-pattern ENABLED: with TinyUSB owning the PHY
    // there is no hardware USB-JTAG-serial unit left, so this hook is the only
    // way 'pio run -t upload' can reach the bootloader over the single cable.
    // Plain terminals cannot produce the exact DTR/RTS sequence by accident.
    if (!USB.begin()) {
        logf("[BOOT] FATAL: USB init failed.\r\n");
    }

    if (!nowLink.begin(chan)) {
        logf("[BOOT] FATAL: ESP-NOW init failed.\r\n");
        led.setState(DeviceState::Connecting);      // solid red = radio dead
    } else {
        logf("[BOOT] Dongle listening on ESP-NOW ch%u\r\n", chan);
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
        logf("[HB] ch=%u peers=%u active=%u fwd=%lu\r\n",
             chan, peerCount, countActivePeers(),
             (unsigned long)forwardedPkts);
    }

    cliPump();

    delay(1);
}
