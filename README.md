# SloppyFirmware

Reliable ESP32-S3 firmware for the SloppyHands glove. Reads all 12 MPR121
electrodes and streams them over UDP to a discovery-based server, with
serial-provisioned WiFi credentials stored in NVS.

## Features

- **Serial WiFi provisioning** — no hard-coded creds. Set via USB CDC serial:
  ```
  wifi set <ssid> <pass>     # store & reboot
  wifi clear                 # erase & reboot
  hand left|right|auto       # mark which hand this device is (v3, stored in NVS)
  status                     # print diagnostics
  reset                      # soft reboot
  help
  ```
  Credentials live in ESP32 NVS (Preferences namespace `wifi`) so they
  survive reboots. If none are stored on first boot the device sits in a
  blue-pulsing `PROVISIONING` state waiting for the `wifi set` command.

- **All 12 MPR121 electrodes streamed every frame.** The server (not the
  device) decides which electrode maps to which finger joint, so you can
  re-assign without re-flashing.

- **SlimeVR-style reliability:**
  - Explicit state machine `PROVISIONING -> CONNECTING -> DISCOVERING -> STREAMING`
  - WiFi event-driven disconnect detection + auto-reconnect
  - Server keepalive watchdog (device falls back to `DISCOVERING`
    if no `KEEPALIVE` arrives within 5 s)
  - Task watchdog (10 s) reboots the chip if the FSM wedges
  - Non-blocking LED state indicator (blue / red / yellow / green)

- **Device-led discovery** — the ESP broadcasts a `HELLO` beacon on the
  local subnet every second; the server replies with a unicast `WELCOME`
  telling the device which data port to stream to and how often to expect
  keepalives.

## Status LED

| Color              | State         | Meaning                                  |
|--------------------|---------------|------------------------------------------|
| Blue pulse         | Provisioning  | No WiFi creds stored; waiting for serial |
| Solid red          | Connecting    | Attempting WiFi association              |
| Yellow pulse       | Discovering   | WiFi up; broadcasting HELLO              |
| Solid green        | Streaming     | Sending DATA frames to server            |

## Wiring

| Signal   | ESP32-S3 pin |
|----------|--------------|
| I2C SDA  | GPIO 8       |
| I2C_SCL  | GPIO 9       |
| WS2812   | GPIO 48      |
| MPR121   | I2C 0x5A     |

I2C runs at 400 kHz. (Adjust in `include/config.h`.)

## Wire protocol

All packets share an 8-byte header for identification:

| Field     | C type   | Size | Notes                                  |
|-----------|----------|------|----------------------------------------|
| magic     | uint32   | 4    | `0x534C5031` ("SLP1")                  |
| type      | uint8    | 1    | 1=HELLO 2=WELCOME 3=DATA 4=KEEPALIVE 5=BYE |
| fwVersion | uint8    | 1    | firmware version (currently `2`)       |
| reserved  | uint16   | 2    | 0                                      |

All multi-byte fields are little-endian; all structs are `__attribute__((packed))`.

### HELLO (device -> broadcast, every 1 s while DISCOVERING) — 18 bytes
```c
struct Header;             // 8
uint8_t  mac[6];           // 6
uint8_t  deviceType;       // 1  (SloppyHands = 1)
uint8_t  channelCount;     // 1  (12)
uint8_t  hand;             // 1  (0=unknown, 1=left, 2=right)  -- v3
uint8_t  reserved;         // 1
```

### WELCOME (server -> device, unicast) — 12 bytes
```c
struct Header;             // 8
uint16_t dataPort;         // 2  UDP port the server wants DATA sent to
uint16_t keepaliveMs;      // 2  cadence at which server will send KEEPALIVE
```

### DATA (device -> server, ~50 FPS) — 48 bytes
```c
struct Header;             // 8
uint32_t packetId;         // 4
uint32_t uptimeMs;         // 4
uint16_t filtered[12];     // 24  MPR121 filtered values
uint16_t touchStatus;      // 2   bitmask of touched electrodes
uint16_t i2cReadTimeMs;    // 2
uint16_t totalLoopTimeMs;  // 2
int8_t   wifiRssi;         // 1
uint8_t  reserved2;        // 1
```

### KEEPALIVE (server -> device) — 12 bytes
```c
struct Header;             // 8
uint32_t lastSeenPacketId; // 4
```

### BYE (either side) — 12 bytes (same shape as KEEPALIVE)
Optional graceful-shutdown packet.

## Server

`test_tracker.py` is the reference server/tracker. It:

1. Binds UDP 4242.
2. Listens for `HELLO` broadcasts from multiple devices simultaneously.
3. Replies with a unicast `WELCOME` (`dataPort=4242`, `keepaliveMs=1000`) per device.
4. Parses incoming `DATA` packets (all 12 electrodes + touch bitmask + RSSI + timing).
5. Sends `KEEPALIVE` once per second to each alive device.
6. **Two hand slots (Left + Right)** — each discovered device shows up in the
   device list with a `hand` dropdown (`auto`/`left`/`right`). Assigning to
   a slot binds that device's 12 channels to that hand. The mapping is
   persisted by MAC in `tracker_config.json` so devices keep their slot
   across reboots.
7. **Per-hand electrode -> joint mapping** — the Settings/Map tabs expose
   dropdowns for every joint. The Right hand defaults to SteamVR thumb+index
   (overridable by assigning an electrode to those joints). The Left hand
   has no SteamVR fusion, so all 10 joints must come from electrodes.
8. **Smoothing** — per-joint pipeline: median filter -> EMA -> deadband.
   Global sliders in the Settings tab.
   - `Snake`/EMA alpha — 0.01 (smooth) .. 1.0 (raw)
   - median window — odd 1..9 (spike rejection)
   - deadband — ignore tiny jitter (0 .. 0.2)
9. Drops devices that go silent for >5 s back to Pending so they re-handshake
   on the next `HELLO`.

Run it:

```
python test_tracker.py
```

Requires SteamVR only if you want right-hand thumb/index fusion; falls back
gracefully. The left hand is always fully ESP32-driven.