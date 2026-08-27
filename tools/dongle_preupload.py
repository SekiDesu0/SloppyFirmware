"""Pre-upload hook for the ESP32-S3 HID dongle (env:esp32s3-dongle).

The dongle exposes one USB composite (vendor HID + CDC). While the app is
running there is no hardware USB-JTAG-serial unit left - TinyUSB owns the PHY -
so esptool cannot use the usual RTS->EN strapping to enter the bootloader.
Instead the app itself watches for esptool's DTR/RTS pattern on the CDC port
(USBCDC reboot hook) and calls usb_persist_restart(RESTART_BOOTLOADER).

That reboot makes the USB device re-enumerate, killing the port out from
under esptool mid-sync: the first 'pio run -t upload' always died with an
I/O error and only a second invocation worked. This hook performs the
pattern ourselves BEFORE esptool runs, waits for the ROM downloader to
re-enumerate, and lets esptool connect on its first try.
"""
Import("env")

import time

VENDOR_ID = 0x303A
APP_PID = 0x534C          # SloppyHands composite (app running)
ROM_PID = 0x1001          # ROM USB-JTAG-serial unit (bootloader)


def _find_port(expected_pids):
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == VENDOR_ID and p.pid in expected_pids:
            return p.device, p.pid
    return None, None


def _knock_into_bootloader(port):
    """Reproduce esptool's DTR/RTS dance that trips USBCDC's reboot hook.

    Required transition order in USBCDC::_onLineState (starting from IDLE):
      !dtr &  rts  ->  dtr &  rts  ->  dtr & !rts  ->  !dtr & !rts => restart
    """
    import serial
    s = serial.Serial(port)
    s.dtr = False
    s.rts = False
    time.sleep(0.05)
    s.rts = True                      # !dtr & rts
    time.sleep(0.05)
    s.dtr = True                      # dtr & rts
    time.sleep(0.05)
    s.rts = False                     # dtr & !rts
    time.sleep(0.05)
    s.dtr = False                     # !dtr & !rts -> bootloader
    time.sleep(0.1)
    try:
        s.close()
    except Exception:
        pass


def pre_upload(source, target, env):
    port, pid = _find_port((APP_PID, ROM_PID))
    if port is None:
        print("(dongle-preup) no dongle port found - letting esptool probe")
        return

    if pid == APP_PID:
        print(f"(dongle-preup) knocking {port} into bootloader...")
        try:
            _knock_into_bootloader(port)
        except Exception as e:
            print(f"(dongle-preup) knock failed ({e}) - continuing anyway")

    # Wait for the ROM downloader to show up (port bounces on re-enumeration).
    deadline = time.time() + 8
    while time.time() < deadline:
        p, cur = _find_port((APP_PID, ROM_PID))
        if p is not None and cur == ROM_PID:
            time.sleep(0.3)           # let udev settle
            print(f"(dongle-preup) bootloader ready on {p}")
            return
        time.sleep(0.25)

    p, cur = _find_port((APP_PID, ROM_PID))
    if p is not None:
        print(f"(dongle-preup) bootloader not seen; device on {p} "
              f"- attempting upload anyway")


env.AddPreAction("upload", pre_upload)
