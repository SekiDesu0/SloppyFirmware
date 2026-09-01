# SloppyFirmware

Reliable ESP32-S3 / ESP8266 firmware for the SloppyHands tracker. Reads all 12
MPR121 electrodes (or a 4-channel FDC2214 capacitance sensor) and streams them
to a server over **two selectable transports**:

- **ESP-NOW** (default) — to a tiny ESP32-S3 USB HID dongle plugged into the PC
- **WiFi UDP** — classic path straight to `test_tracker.py` on the local network

with serial-provisioned WiFi credentials stored in NVS for the WiFi path.

> **What is a SloppyHands?** It is a **hand tracker** — a small sensor board
> you attach to your VR controller so the flex of your fingers can drive finger
> tracking. It is **not a glove**: nothing is worn over the hand, and the device
> itself does not move your fingers. The tracker measures finger flex; the
> server maps that flex to joints and (optionally) feeds SteamVR/other
> pipelines. Throughout this document "tracker" and "device" are the same
> thing.

## Topology

```
[tracker L]──┐                          ┌─ USB HID input reports ─▶ test_tracker.py --source hid
            ├── ESP-NOW ─▶ [S3 dongle]─┤
[tracker R]──┘   auto-pair              └─ USB CDC serial console ─▶ CLI / status
```

Trackers hop Wi-Fi channels broadcasting HELLO beacons; the dongle listens on its
fixed channel and answers WELCOME. The pairing (dongle MAC + channel) is stored
in NVS so later boots reconnect instantly via unicast. More than two trackers
works — the dongle keeps a peer table (`ESPNOW_MAX_PEERS`, default 8).

## Features

- **Serial WiFi provisioning** — no hard-coded creds. Set via USB CDC serial:
  ```
  wifi set <ssid> <pass>     # store & reboot (WiFi transport only)
  wifi clear                 # erase & reboot
  transport espnow|wifi      # pick ESP-NOW dongle or classic UDP (reboots)
  pair clear                 # forget the paired dongle
  hand left|right|auto       # mark which hand this device is (stored in NVS)
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
  the same wire packets over ESP-NOW instead of UDP. On the tracker side a
  channel-hopping discovery finds the dongle, remembers its MAC/channel, then
  streams DATA unicasts. See "Dongle firmware" below.

## Building & flashing the tracker firmware

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

# ESP32-S3 tracker
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
- **Serial console** on the same USB cable (the dongle's `ttyACM` port; open it
  and the banner + prompt appear immediately):
  ```
  status        fw / channel / peer count / forwarded packets
  list          known tracker peers with age, RSSI and last packet id
  channel <n>   set ESP-NOW Wi-Fi channel (persisted in NVS, reboots)
  forget        drop the in-RAM peer table; trackers re-pair on next HELLO
  reset | help
  ```
- Log output (`[HB]` heartbeat every 5 s: `ch=` channel, `peers=`/`active=`
  counts, `fwd=` forwarded packets) mirrors to the console **and UART0**
  (TXD0/RXD0 pins — bench escape hatch only).
- VID/PID: `0x303A`/`0x534C`.

### Building & flashing

```sh
pio run -e esp32s3-dongle -t upload
```

A pre-upload hook (`tools/dongle_preupload.py`) puts the running app into the
bootloader automatically — no manual reset needed (TinyUSB owns the PHY, so the
usual hardware auto-reset path doesn't exist here).

On the PC, run the tracker against the dongle:

```
pip install hidapi
python test_tracker.py --source hid          # --vid/--pid to override matching
```

Tracker-side commands of interest: `transport espnow`, `pair clear`, `status`
(shows link/dongle/RSSI). To move everything to another channel, flash the
dongle with the new channel, then `pair clear` on each tracker so they re-hop
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

- **Optional battery gauge** — an ADC pin behind a resistor divider reads
  the Li-ion cell every frame:
  ```
  VBAT --[R_TOP]--+--[R_BOT]-- GND
                  |
               ADC pin
  ```
  Pin, divider values and empty/full voltage are all set in
  `include/config.h` (`BATT_ADC_PIN`, `BATT_DIVIDER_R_TOP/BOT`,
  `BATT_EMPTY_VOLTS`, `BATT_FULL_VOLTS`). The tracker streams divider-compensated
  millivolts plus a smoothed percent in every DATA frame (`255` percent =
  disabled/unwired); the tracker shows it in the device table's **Batt**
  column. ESP32-S3 default pin is GPIO4 — ADC1 only, since ADC2 can't be read
  while the WiFi radio runs. ESP8266 uses A0 (onboard board divider assumed).
  Set `BATT_ADC_PIN < 0` to build without it.

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
| Battery ADC (divider tap) | GPIO 4 |

I2C runs at 400 kHz. (Adjust in `include/config.h`.)

## Wire protocol

This is the complete, versioned wire format. It is stable enough to implement
from scratch in any language; `lib/PacketIO/PacketIO.h` (firmware) and the
constants at the top of `test_tracker.py` (Python) are the two reference
implementations.

### Conventions

- **Endianness:** all multi-byte fields are **little-endian**.
- **Packing:** every struct is `__attribute__((packed))` (C) / `<` format
  string (Python `struct`) — no padding between fields.
- **Magic:** every packet starts with `0x534C5031` ("SLP1").
- **`fwVersion`:** currently `6`. Always check the magic and type before
  trusting the rest of a packet.

### Common header — 8 bytes

| Field     | C type   | Size | Notes                                  |
|-----------|----------|------|----------------------------------------|
| magic     | uint32   | 4    | `0x534C5031` ("SLP1")                  |
| type      | uint8    | 1    | 1=HELLO 2=WELCOME 3=DATA 4=KEEPALIVE 5=BYE 6=TUNNEL |
| fwVersion | uint8    | 1    | firmware version (currently `6`)       |
| reserved  | uint16   | 2    | 0                                      |

### Python `struct` reference

Copy these verbatim; they mirror `test_tracker.py`:

```python
import struct
MAGIC = 0x534C5031  # "SLP1"

HEADER_FMT   = "<IBBH"                    # magic, type, fwVersion, reserved
HELLO_FMT    = "<IBBH6sBBBB"              # + mac(6s) deviceType channelCount hand reserved
WELCOME_FMT  = "<IBBHH"                   # + dataPort keepaliveMs
DATA_FMT     = "<IBBII12HH4IBBHHbBHB"     # full 69-byte sensor frame (below)
KEEP_FMT     = "<IBBI"                    # + lastSeenPacketId
TUNNEL_FMT   = "<IBBH6sbB"                # + mac(6s) rssi reserved, then an embedded DATA

HELLO_LEN    = struct.calcsize(HELLO_FMT)    # 18
WELCOME_LEN  = struct.calcsize(WELCOME_FMT)  # 12
DATA_LEN     = struct.calcsize(DATA_FMT)     # 69
KEEP_LEN     = struct.calcsize(KEEP_FMT)     # 12
TUNNEL_LEN   = struct.calcsize(TUNNEL_FMT) + DATA_LEN   # 16 + 69 = 85
```

### Packet types

#### HELLO (tracker -> broadcast, every 1 s while DISCOVERING) — 18 bytes
```c
struct Header;             // 8
uint8_t  mac[6];           // 6
uint8_t  deviceType;       // 1  (SloppyHands = 1)
uint8_t  channelCount;     // 1  (12; informational — DATA.sensorType is authoritative)
uint8_t  hand;             // 1  (0=unknown, 1=left, 2=right)
uint8_t  reserved;         // 1
```

#### WELCOME (server -> tracker, unicast) — 12 bytes
```c
struct Header;             // 8
uint16_t dataPort;         // 2  UDP port the server wants DATA sent to (unused over ESP-NOW)
uint16_t keepaliveMs;      // 2  cadence at which server will send KEEPALIVE
```

#### DATA (tracker -> server, ~50 FPS) — 69 bytes
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
uint16_t battMv;           // 2   battery cell mV (divider-compensated; 0 = unknown)
uint8_t  battPercent;      // 1   0..100 (255 = unknown/disabled)
```

`field` order note for Python unpacking: after the header the tuple is
`(packetId, uptimeMs, filtered[12], touchStatus, fdcRaw[4], sensorType,
reserved2, i2cReadTimeMs, totalLoopTimeMs, wifiRssi, reserved3, battMv,
battPercent)`.

`sensorType` selects which payload is live: `0` = none, `1` = MPR121
(`filtered` + `touchStatus`), `2` = FDC2214 (`fdcRaw`). The inactive payload is
zero-filled.

`fdcRaw` is the raw 28-bit `DATAx` register value = `fSENSOR / fREF × 2^28`
(≈14 M corresponds to a ~2.1 MHz sensor oscillation). It *decreases* as
capacitance rises, so the server computes flex as `baseline − raw`.

#### KEEPALIVE (server -> tracker) — 12 bytes
```c
struct Header;             // 8
uint32_t lastSeenPacketId; // 4
```

#### BYE (either side) — 12 bytes (same shape as KEEPALIVE)
Optional graceful-shutdown packet.

#### TUNNEL (dongle -> PC, one USB HID report) — 85 bytes
```c
struct Header;             // 8   type = 6
uint8_t  mac[6];           // 6   source tracker MAC
int8_t   rssi;             // 1   ESP-NOW rx RSSI measured by the dongle
uint8_t  reserved;         // 1
DataPacket data;           // 69  embedded DATA frame
```
The dongle wraps every tracker DATA frame so the PC-side tracker can identify
which tracker it came from (over UDP the sender IP plays that role). The same
packets ride over both transports unchanged; only TUNNEL is HID-specific.
Because a full-speed HID endpoint caps at 64 bytes per transaction, each
TUNNEL frame travels as two input reports: **ID 1 = first 62 bytes,
ID 2 = remaining 23.**

> **HID reassembly caveat (Windows):** a full-speed HID class driver pads every
> report to the largest report size (62 bytes), so report ID 2 arrives as **63
> bytes** on Windows (23 payload bytes + 40 padding), while hidraw on Linux
> returns it at its exact 23-byte size. Always slice report ID 2 to
> `TUNNEL_LEN - 62 = 23` bytes, and only reassemble when you have a full 62-byte
> part 1 buffered (see `test_tracker.py` `ingest_hid_reports`).

### Handshake / session sequence

1. **Discovery** — tracker broadcasts `HELLO` (every 1 s over UDP; every 300 ms
   while channel-hopping over ESP-NOW) until it hears a `WELCOME`.
2. **Accept** — the server/dongle replies `WELCOME` unicast with `keepaliveMs`
   (and `dataPort` for UDP). The tracker latches the sender (IP or dongle MAC)
   and enters STREAMING.
3. **Stream** — tracker sends `DATA` at ~50 FPS.
4. **Keepalive** — server/dongle sends `KEEPALIVE` every `keepaliveMs`
   (default 1000 ms), echoing `lastSeenPacketId`. The tracker resets its
   5 s watchdog on each one.
5. **Timeout** — if no `KEEPALIVE` arrives for 5 s, the tracker drops the
   session and returns to DISCOVERY (or CONNECTING on WiFi loss).
6. **Shutdown (optional)** — either side may send `BYE` to end a session.

## Calibration

Flex is derived from **capacitance**: both sensors report a value that
*decreases* as capacitance rises (a finger bending toward the electrode adds
capacitance). So the flex amount is always a **difference from a resting
baseline**:

```
flex = clamp((baseline - raw) / delta, 0..1)
```

- **FDC2214** `raw` is the 28-bit conversion result = `fSENSOR / fREF × 2^28`
  (≈14 M at rest ≈2.1 MHz); it drops as a finger approaches.
- **MPR121** `filtered` is the 16-bit filtered electrode value; it also drops
  on touch/flex.

### Per-channel calibration tables

Each hand stores its own `{baseline, flexed, delta}` table, one entry per
channel (4 for FDC2214, 12 for MPR121). They are persisted per hand in
`tracker_config.json` under `hands.left.fdc_cal`, `hands.left.mpr_cal`,
`hands.right.fdc_cal`, `hands.right.mpr_cal`:

```json
"fdc_cal": { "baseline": [0, 0, 0, 0], "flexed": [0, 0, 0, 0], "delta": [0, 0, 0, 0] },
"mpr_cal": { "baseline": [0, ...12],  "flexed": [0, ...12],  "delta": [0, ...12] }
```

### Capture flow (per hand, per sensor)

1. **Set Rest** — with the hand open, the tracker averages that hand's live
   channels for 0.6 s and stores the result as `baseline`.
2. **Set Flex** — make a fist; the tracker averages for 0.6 s into `flexed`,
   then computes per channel:
   ```
   delta = max(0, baseline - flexed)
   ```
3. **Normalize** — at runtime each joint maps to a channel and applies
   `flex = clamp((baseline[ch] - raw[ch]) / delta[ch], 0..1)`.

A channel with `delta <= 0` is **uncalibrated**:

- **MPR121** falls back to the global **Baseline** / **Max Delta** sliders:
  ```
  norm = clamp((baseline - raw) / max_delta, 0..1)
  ```
- **FDC2214** reports `0` flex (no global fallback).

The GUI has a `FDC Calibrate` and an `MPR Calibrate` tab with per-hand
`Set Rest` / `Set Flex` / `Reset Cal` buttons; `Reset Cal` zeroes that hand's
table again.

### Smoothing

After normalization each joint runs through the same pipeline
(`Smoother.update` in `test_tracker.py`):

1. **Median filter** over a window of `median_window` (odd, 1..9) samples —
   rejects single-sample spikes.
2. **EMA** — `smoothed = alpha * median + (1 - alpha) * smoothed`, with
   `ema_alpha` from 0.01 (very smooth) to 1.0 (raw).
3. **Deadband** — if `|smoothed - last_output| < deadband` (0..0.2), hold the
   previous output to ignore tiny jitter.

## Server

`test_tracker.py` is the reference server/tracker — a dev tool for exercising
the ESP firmware (receive its frames over UDP or the HID dongle, visualize the
hands, and calibrate the sensors). It is **not** a production consumer. It:

1. Binds UDP 4242 (`--source udp`) **or** reads TUNNEL reports from
   the dongle's HID interface (`--source hid`). The source is also a pair of
   radio buttons in the GUI and can be switched while running.
2. Listens for `HELLO` broadcasts from multiple devices simultaneously.
3. Replies with a unicast `WELCOME` (`dataPort=4242`, `keepaliveMs=1000`) per device.
4. Parses incoming `DATA` packets (12 MPR121 electrodes + touch bitmask, or 4 FDC2214 raw channels, + RSSI + timing + battery).
5. Sends `KEEPALIVE` once per second to each alive device.
6. **Two hand slots (Left + Right)** — each discovered device shows up in the
   device list with a `hand` dropdown (`auto`/`left`/`right`). Assigning to
   a slot binds that device's channels to that hand. The mapping is
   persisted by MAC in `tracker_config.json` so devices keep their slot
   across reboots.
7. **Per-hand electrode -> joint mapping** — the Settings/Map tabs expose
   dropdowns for every joint. Both hands fuse SteamVR thumb+index from the
   attached VR controller (overridable per joint by assigning an electrode to
   it); the middle finger also falls back to the controller grip when its
   electrodes are unmapped and read zero. See "SteamVR fusion" below.
8. **FDC2214 mapping + calibration** — the map tabs expose a second `FDC`
   column so each joint maps to channel `0..3` (same channel on proximal+distal
   = one electrode per finger; different channels = two). A `FDC Calibrate` tab
   captures a resting `baseline` and a flexed value per channel, then the
   tracker normalizes `flex = clamp((baseline − raw) / delta, 0..1)`, driving
   the bars and skeleton fingers.
9. **MPR121 calibration** — an `MPR Calibrate` tab does the same per-channel
   rest/flex capture across all 12 electrodes (`mpr_cal` in
   `tracker_config.json`). Channels with a captured delta normalize through
   it; uncalibrated channels keep falling back to the global Baseline /
   Max Delta sliders. Both calibrate tabs have a `Reset Cal` button per hand
   that zeroes that hand's captured table again.
10. **Smoothing** — per-joint pipeline: median filter -> EMA -> deadband.
    Global sliders in the Settings tab.
    - EMA alpha — 0.01 (smooth) .. 1.0 (raw)
    - median window — odd 1..9 (spike rejection)
    - deadband — ignore tiny jitter (0 .. 0.2)
11. Drops devices that go silent for >5 s back to Pending so they re-handshake
    on the next `HELLO`.
12. **Runtime source switching + dongle hotplug** — radio buttons above the
    device list pick `WiFi UDP` vs `ESP-NOW dongle` while the tracker is
    running; the choice persists in `tracker_config.json` (`--source` remains
    a boot override). In dongle mode an unplugged USB dongle turns the status
    line red (`[DONGLE OFFLINE]`) and skeletons drop out via the normal 5 s
    timeout; plugging it back in reconnects automatically and trackers resume
    their saved hand slots. Starting in HID mode with no dongle attached also
    works: the tracker waits for it instead of exiting.
13. **Battery column** — trackers report cell millivolts + percent in every
    frame; the device table's `Batt` column (and the diagnostics line) show the
    live percentage. A disabled/unwired monitor displays `-`.

### SteamVR fusion

When SteamVR is running, `test_tracker.py` fuses the attached VR controller's
thumb + index input with the tracker's electrode data, for **both hands**. The
middle finger falls back to the controller grip; ring/pinky are always
electrode-driven.

| Joint  | Controller input                                                          |
|--------|---------------------------------------------------------------------------|
| Index  | trigger axis (touch = 0.2, pull = axis value)                             |
| Thumb  | A (right) / X (left), B (right) / Y (left), or the thumbstick (stick X = thumb angle, stick Y = curl) |
| Middle | grip button (fallback only — when both mid joints are unmapped and read zero) |

- **Electrode override** — mapping an electrode to `thumb_p/d` or `index_p/d`
  makes that joint electrode-driven, overriding the SteamVR input for it.
- Thumb/index are **unmapped by default**, so fusion is on out of the box; the
  left hand's thumb pose and curl are mirrored to match its geometry.
- Requires SteamVR running (`VRApplication_Scene`); without it the tracker
  degrades gracefully to electrode-only tracking for every joint.

Run it:

```
python test_tracker.py [--source udp|hid]
```

Requires SteamVR only if you want thumb/index fusion (both hands); falls back
gracefully to electrode-only tracking when SteamVR isn't running.