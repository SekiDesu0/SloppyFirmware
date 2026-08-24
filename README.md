# SloppyFirmware

Reliable ESP32-S3 / ESP8266 firmware for the SloppyHands glove. Reads all 12
MPR121 electrodes (or a 4-channel FDC2214 capacitance sensor) and streams them
to a server over **two selectable transports**:

- **ESP-NOW** (default) — to a tiny ESP32-S3 USB HID dongle plugged into the PC
- **WiFi UDP** — classic path straight to `test_tracker.py` on the local network

with serial-provisioned WiFi credentials stored in NVS for the WiFi path.

## Topology

```
[glove L]──┐                          ┌─ USB HID input reports ─▶ test_tracker.py --source hid
           ├── ESP-NOW ─▶ [S3 dongle]─┤
[glove R]──┘   auto-pair              └─ USB CDC console (+ UART0 logs) ─▶ status output
```

Gloves hop Wi-Fi channels broadcasting HELLO beacons; the dongle listens on its
fixed channel and answers WELCOME. The pairing (dongle MAC + channel) is stored
in NVS so later boots reconnect instantly via unicast. More than two gloves
works — the dongle keeps a peer table (`ESPNOW_MAX_PEERS`, default 8).

## Features

- **Serial WiFi provisioning** — no hard-coded creds. Set via USB CDC serial:
  ```
  wifi set <ssid> <pass>     # store & reboot (WiFi transport only)
  wifi clear                 # erase & reboot
  transport espnow|wifi      # pick ESP-NOW dongle or classic UDP (reboots)
  pair clear                 # forget the paired dongle
  hand left|right|auto       # mark which hand this device is (v3, stored in NVS)
  sensor auto|mpr121|fdc2214 # select sensor (auto probes FDC2214 then MPR121)
  status                     # print diagnostics
  reset                      # soft reboot
  help
  ```
  Credentials live in ESP32 NVS / ESP8266 EEPROM so they survive reboots. If
  none are stored on first boot the device sits in a blue-pulsing
  `PROVISIONING` state waiting for the `wifi set` command. The ESP-NOW path
  needs no credentials at all.

- **ESP-NOW transport + HID dongle** — `transport espnow` (the default) sends
  the same wire packets over ESP-NOW instead of UDP. On the glove side a
  channel-hopping discovery finds the dongle, remembers its MAC/channel, then
  streams DATA unicasts. See "Dongle firmware" below.

## Building & flashing the glove firmware

Prerequisites: [PlatformIO](https://docs.platformio.org/en/latest/core/installation.html)
(`pip install platformio` or the VS Code extension). No extra setup needed —
library versions are resolved from `platformio.ini` on first build.

Pick the environment matching your board:

| Board | PlatformIO env | Notes |
|-------|----------------|-------|
| ESP32-S3 DevKitC-1 / SuperMini | `esp32-s3-devkitc-1` | built-in WS2812 on GPIO48 |
| NodeMCU (ESP8266)              | `esp8266-nodemcuv2`  | needs external NeoPixel |
| Wemos D1 Mini (ESP8266)        | `d1_mini`            | needs external NeoPixel |

### Platform notes

Both ESP32-S3 environments pin [pioarduino](https://github.com/pioarduino/platform-espressif32)
— the community platform that ships arduino-esp32 3.x (IDF 5.x) — via its
rolling `stable` release; official PlatformIO `espressif32` stopped at Arduino
core 2.x. The ESP8266 environments use stock `espressif8266`. The dongle uses
only the **core's** TinyUSB stack; do not add the external Adafruit TinyUSB
library, or two copies of the USB stack collide (duplicate-symbol build
crashes, most visibly on Windows).

Compile and upload (plug the board in via USB; `PORT` is optional — PlatformIO
auto-detects):

```sh
# NodeMCU
pio run -e esp8266-nodemcuv2 -t upload            # add --upload-port COM5 (Windows) or /dev/ttyUSB0 (Linux)

# Wemos D1 Mini
pio run -e d1_mini -t upload

# ESP32-S3 glove
pio run -e esp32-s3-devkitc-1 -t upload
```

Then open the serial console to provision (`Ctrl+T`, `Ctrl+T`, `Ctrl+H` style
hotkeys are not needed — just type):

```sh
pio device monitor -e esp8266-nodemcuv2 -b 115200   # same for d1_mini / esp32-s3-devkitc-1
```

On first boot the LED pulses blue waiting for provisioning. For the WiFi/UDP
transport run `wifi set <ssid> <pass>`; for the dongle path just leave the
default (`transport espnow`) — no credentials needed.

### ESP8266 wiring (NodeMCU / D1 Mini)

| Signal   | NodeMCU label | D1 Mini label | GPIO     |
|----------|---------------|---------------|----------|
| I2C SDA  | D2            | D2            | GPIO4    |
| I2C SCL  | D1            | D1            | GPIO5    |
| NeoPixel | D6            | D6            | GPIO12   |

The NeoPixel data line must be an **external** WS2812 chain on GPIO12 — the
on-board LED of these boards is a plain GPIO LED, not addressable. Pins live in
`include/config.h` if your wiring differs.

## Dongle firmware (`env:esp32s3-dongle`)

A separate firmware image for an ESP32-S3 board that plugs into the PC:

- **Native USB composite** — the Arduino core's own TinyUSB stack (USBHID
  classes), no external TinyUSB library. A vendor-defined **HID** interface
  carries TUNNEL frames (DATA packet + source MAC + dongle RSSI) while a
  **CDC serial interface** mirrors the log output, so a console on the
  dongle's `ttyACM` port stays live while it streams. `ARDUINO_USB_CDC_ON_BOOT`
  must stay `0`: setting it to 1 makes the core auto-start USB with default
  descriptors before `setup()` and enumeration never happens.
- There is **no interactive CLI**: logs print to the CDC console and UART0,
  including a `[HB]` heartbeat every 5 s (`ch=` channel, `peers=`/`active=`
  counts, `fwd=` forwarded packets). The Wi-Fi channel is persisted in NVS; to
  change it, flash the dongle with the desired default (enter the loader via
  BOOT+RST) or erase NVS.
- VID/PID: `0x303A`/`0x534C`. Flash it with `pio run -e esp32s3-dongle -t upload`.

On the PC, run the tracker against the dongle:

```
pip install hidapi
python test_tracker.py --source hid          # --vid/--pid to override matching
```

Glove-side commands of interest: `transport espnow`, `pair clear`, `status`
(shows link/dongle/RSSI). To move everything to another channel, flash the
dongle with the new channel, then `pair clear` on each glove so they re-hop
from their default and re-pair.

- **All 12 MPR121 electrodes streamed every frame.** The server (not the
  device) decides which electrode maps to which finger joint, so you can
  re-assign without re-flashing.

- **Optional FDC2214 support** — swap the sensor front-end to a 4-channel,
  28-bit FDC2214 capacitance-to-digital converter. The driver auto-detects the
  I2C address (`0x2A`/`0x2B`, selected by the ADDR pin) and configures the chip
  into sequential auto-scan mode (one channel excited/read at a time). Selected
  via `sensor auto|mpr121|fdc2214` (persisted in NVS) or the compile-time
  `SENSOR_MODE_DEFAULT` in `include/config.h`. Register-level tuning
  (`RCOUNT`, `SETTLECOUNT`, `DRIVE_CURRENT`/IDRIVE) lives in `include/config.h`;
  the internal reference oscillator is used, `SD` must be tied low, and `INTB`
  is left unconnected (data is polled over I2C).

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
| Purple SOS (morse) | Sensor absent | No MPR121/FDC2214 detected on the I2C bus |

The sensor-absent pattern overrides all other states: the LED blinks `...`
`---` `...` in purple until a sensor is found (re-probed on reboot, or switch
the active sensor with `sensor auto|mpr121|fdc2214`).

## Wiring

| Signal   | ESP32-S3 pin |
|----------|--------------|
| I2C SDA  | GPIO 8       |
| I2C_SCL  | GPIO 9       |
| WS2812   | GPIO 48      |
| MPR121   | I2C 0x5A     |
| FDC2214  | I2C 0x2A/0x2B |

I2C runs at 400 kHz. (Adjust in `include/config.h`.)

## Wire protocol

All packets share an 8-byte header for identification:

| Field     | C type   | Size | Notes                                  |
|-----------|----------|------|----------------------------------------|
| magic     | uint32   | 4    | `0x534C5031` ("SLP1")                  |
| type      | uint8    | 1    | 1=HELLO 2=WELCOME 3=DATA 4=KEEPALIVE 5=BYE 6=TUNNEL |
| fwVersion | uint8    | 1    | firmware version (currently `5`)       |
| reserved  | uint16   | 2    | 0                                      |

All multi-byte fields are little-endian; all structs are `__attribute__((packed))`.

### HELLO (device -> broadcast, every 1 s while DISCOVERING) — 18 bytes
```c
struct Header;             // 8
uint8_t  mac[6];           // 6
uint8_t  deviceType;       // 1  (SloppyHands = 1)
uint8_t  channelCount;     // 1  (12; informational — DATA.sensorType is authoritative)
uint8_t  hand;             // 1  (0=unknown, 1=left, 2=right)  -- v3
uint8_t  reserved;         // 1
```

### WELCOME (server -> device, unicast) — 12 bytes
```c
struct Header;             // 8
uint16_t dataPort;         // 2  UDP port the server wants DATA sent to
uint16_t keepaliveMs;      // 2  cadence at which server will send KEEPALIVE
```

### DATA (device -> server, ~50 FPS) — 66 bytes
```c
struct Header;             // 8
uint32_t packetId;         // 4
uint32_t uptimeMs;         // 4
uint16_t filtered[12];     // 24  MPR121 filtered values (zero when inactive)
uint16_t touchStatus;      // 2   MPR121 touch bitmask
uint32_t fdcRaw[4];        // 16  FDC2214 28-bit raw (zero when inactive)
uint8_t  sensorType;       // 1   0=none, 1=MPR121, 2=FDC2214
uint8_t  reserved2;        // 1
uint16_t i2cReadTimeMs;    // 2
uint16_t totalLoopTimeMs;  // 2
int8_t   wifiRssi;         // 1
uint8_t  reserved3;        // 1
```

`sensorType` selects which payload is live: `0` = none, `1` = MPR121
(`filtered` + `touchStatus`), `2` = FDC2214 (`fdcRaw`). The inactive payload is
zero-filled.

`fdcRaw` is the raw 28-bit `DATAx` register value = `fSENSOR / fREF × 2^28`
(≈14 M corresponds to a ~2.1 MHz sensor oscillation). It *decreases* as
capacitance rises, so the server computes flex as `baseline − raw`.

### KEEPALIVE (server -> device) — 12 bytes
```c
struct Header;             // 8
uint32_t lastSeenPacketId; // 4
```

### BYE (either side) — 12 bytes (same shape as KEEPALIVE)
Optional graceful-shutdown packet.

### TUNNEL (dongle -> PC, one USB HID report) — 82 bytes
```c
struct Header;             // 8   type = 6
uint8_t  mac[6];           // 6   source glove MAC
int8_t   rssi;             // 1   ESP-NOW rx RSSI measured by the dongle
uint8_t  reserved;         // 1
DataPacket data;           // 66  embedded DATA frame
```
The dongle wraps every glove DATA frame so the PC-side tracker can identify
which glove it came from (over UDP the sender IP plays that role). The same
packets ride over both transports unchanged; only TUNNEL is HID-specific.

## Server

`test_tracker.py` is the reference server/tracker. It:

1. Binds UDP 4242 (`--source udp`, default) **or** reads TUNNEL reports from
   the dongle's HID interface (`--source hid`).
2. Listens for `HELLO` broadcasts from multiple devices simultaneously.
3. Replies with a unicast `WELCOME` (`dataPort=4242`, `keepaliveMs=1000`) per device.
4. Parses incoming `DATA` packets (12 MPR121 electrodes + touch bitmask, or 4 FDC2214 raw channels, + RSSI + timing).
5. Sends `KEEPALIVE` once per second to each alive device.
6. **Two hand slots (Left + Right)** — each discovered device shows up in the
   device list with a `hand` dropdown (`auto`/`left`/`right`). Assigning to
   a slot binds that device's channels to that hand. The mapping is
   persisted by MAC in `tracker_config.json` so devices keep their slot
   across reboots.
7. **Per-hand electrode -> joint mapping** — the Settings/Map tabs expose
   dropdowns for every joint. The Right hand defaults to SteamVR thumb+index
   (overridable by assigning an electrode to those joints). The Left hand
   has no SteamVR fusion, so all 10 joints must come from electrodes.
8. **FDC2214 mapping + calibration** — the map tabs expose a second `FDC`
   column so each joint maps to channel `0..3` (same channel on proximal+distal
   = one electrode per finger; different channels = two). A `FDC Calibrate` tab
   captures a resting `baseline` and a flexed value per channel, then the
   tracker normalizes `flex = clamp((baseline − raw) / delta, 0..1)`, driving
   the bars and skeleton fingers.
9. **Smoothing** — per-joint pipeline: median filter -> EMA -> deadband.
   Global sliders in the Settings tab.
   - `Snake`/EMA alpha — 0.01 (smooth) .. 1.0 (raw)
   - median window — odd 1..9 (spike rejection)
   - deadband — ignore tiny jitter (0 .. 0.2)
10. Drops devices that go silent for >5 s back to Pending so they re-handshake
    on the next `HELLO`.

Run it:

```
python test_tracker.py
```

Requires SteamVR only if you want right-hand thumb/index fusion; falls back
gracefully. The left hand is always fully ESP32-driven.