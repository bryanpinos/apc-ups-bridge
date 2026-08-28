# apc-ups-bridge

Read an **APC UPS over USB HID** with an ESP32-S3 and republish it to **MQTT** with
Home Assistant auto-discovery.

No host computer involved. The UPS plugs into the ESP32, not into your NAS or server,
which frees the USB port and removes the "my UPS monitor died because the machine it
was plugged into went down" problem.

```
APC UPS ──USB──> MAX3421E FeatherWing ──SPI──> ESP32-S3 ──WiFi──> MQTT ──> Home Assistant
```

## Why

The usual way to monitor a UPS is NUT or apcupsd on a machine plugged into it. That
works, but it consumes a USB port on the very machine the UPS exists to protect, and
the monitoring dies exactly when the host does.

This project started after a UPS failed with a genuine internal fault and announced it
by beeping — because nothing was watching it. A battery was replaced that didn't need
replacing. Every fault flag this firmware exposes (`NeedReplacement`, `Overload`,
`ShutdownImminent`) was available over USB the whole time.

## What it publishes

| Kind | Entities |
|---|---|
| Sensors | battery %, runtime (s), load %, input voltage, battery voltage, self-test result |
| Binary sensors | on mains, on battery, needs replacement, overload, shutdown imminent, below capacity limit |
| Diagnostics | WiFi RSSI, uptime, free heap, USB device VID:PID, USB attached, HID mounted, board battery |

Plus the raw HID report descriptor on `<base>/hid/descriptor`, which is how you adapt
it to a UPS this code has never seen.

## Hardware

| Part | Notes |
|---|---|
| **Adafruit ESP32-S3 Feather** (8 MB, no PSRAM) | Any ESP32-**S2/S3** Feather works. Classic ESP32 does **not** — Adafruit TinyUSB requires S2/S3. |
| **Adafruit USB Host FeatherWing (MAX3421E)** | CS = pin 10, IRQ = pin 9, fixed. Onboard 5 V/1 A boost supplies VBUS. |
| USB cable from the UPS | APC ships an RJ50-to-USB-A cable; it plugs straight into the wing's USB-A jack. |
| LiPo cell *(optional)* | Lets the monitor outlive the UPS it is monitoring. The Feather's MAX17048 reports its own charge. |

Stack the wing on the Feather. There is no other wiring.

**Why the wing instead of the S3's native USB host?** The ESP32-S3's USB-OTG and
USB-Serial-JTAG share a single PHY, so using native host mode costs you the serial
console. The MAX3421E runs over SPI and leaves native USB free.

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
cp src/secrets.h.example src/secrets.h   # then edit it
pio run -e feather -t upload             # first flash over USB
```

After the first flash everything is over the air:

```bash
export OTA_HOST=192.168.0.50   # the board's IP
export OTA_PW=your-ota-password
pio run -e ota -t upload
```

`secrets.h` is gitignored and holds WiFi, MQTT, OTA and the per-unit identity.

### Running more than one

Set a distinct `UNIT_ID` / `UNIT_HOST` / `UNIT_NAME` per board in `secrets.h`.
Home Assistant derives entity_ids from **`UNIT_NAME`**, not `UNIT_ID`, so pick it
deliberately — renaming later renames every entity.

## MQTT topics

Base topic is `apcups/<UNIT_ID>/`.

| Topic | Payload |
|---|---|
| `status` | `online` / `offline` (LWT, retained) |
| `ups` | JSON: all decoded UPS state |
| `diag` | JSON: RSSI, uptime, heap, IP, hostname, USB state |
| `boot` | JSON: reset reason, firmware version (retained) |
| `stage` | boot-stage beacon (retained) |
| `hid/descriptor` | raw HID report descriptor, hex (retained) |
| `log` | free-text log lines |
| `cmd` | accepts `dump`, `diag`, `discovery`, `reboot` |

## Adapting to a different UPS

**The report map is model-specific — do not assume this one fits your UPS.** The
firmware dumps the descriptor unprompted, so adapting is mechanical:

```bash
mosquitto_sub -h broker -t 'apcups/<unit>/hid/descriptor' -C 1 > mine.hex
python3 tools/parse_hid.py mine.hex
```

`parse_hid.py` prints every field as *report id / bit offset / bit size / usage name /
logical range*. Then edit `POLL[]` and `applyReport()` in `src/main.cpp` to match.

### Reference map: APC Back-UPS Pro BR1500MS2 (`051d:0002`)

```
Rpt 22 (2B)  PresentStatus bitfield -- the entire status word in one read
   bit0 Charging   bit1 Discharging  bit2 ACPresent  bit3 BatteryPresent
   bit4 BelowRemainingCapacityLimit  bit5 ShutdownImminent
   bit6 RemainingTimeLimitExpired    bit7 CommunicationLost
   bit8 NeedReplacement              bit9 Overload   bit10 VoltageNotRegulated
Rpt 12 (3B)  RemainingCapacity (8b) + RunTimeToEmpty (16b, seconds)
Rpt 80 (1B)  PercentLoad %
Rpt 33 (1B)  Test result: 1 passed, 2 warning, 3 error, 4 aborted,
                          5 in progress, 6 no test initiated, 7 scheduled
Rpt 49 (2B)  Input voltage, volts
Rpt  9 (2B)  Battery voltage, raw/100 (24 V pack reads ~2730 -> 27.30 V)
Rpt 24 (1B)  AudibleAlarmControl 1..3
```

Battery voltage scaling differs by pack: a 12 V unit and a 24 V unit both report
hundredths, so check the value against the nominal pack voltage rather than trusting
the HID unit exponent, which APC populates oddly.

## Gotchas

Everything here cost real debugging time. If you are building something similar,
read this section first.

**`USBHost.task()` blocks forever by default.** The signature is
`task(uint32_t timeout_ms = UINT32_MAX, ...)`. Under FreeRTOS it sleeps until a USB
event arrives, so with nothing attached the first call in `loop()` never returns.
WiFi stays associated and pings still answer — because that is a different task — so
it looks like a network bug. **Always call `USBHost.task(0)`.**

**TinyUSB silently refuses report descriptors over 256 bytes.** `hid_host.c` checks
`report_desc_len > CFG_TUH_ENUMERATION_BUFSIZE` and calls the mount callback with
`NULL, 0`. That constant is not `#ifndef`-guarded, so `-D` cannot override it. APC
descriptors are ~1 kB. Fetch it yourself with the public
`tuh_descriptor_get_hid_report()` into your own buffer.

**Use the async USB APIs, never the `_sync` variants.** `loop()` is what calls
`USBHost.task(0)`, so a blocking sync call from `loop()` deadlocks the stack — nothing
is left to service it. Report polling is a one-request-in-flight state machine
advanced from `tuh_hid_get_report_complete_cb()`.

**`-DARDUINO_USB_MODE=1` is required.** Otherwise the Arduino core links its own
`libarduino_tinyusb.a` and you get dozens of `multiple definition of 'usbd_*'` link
errors against Adafruit TinyUSB.

**After an esptool USB flash, an ESP32-S3 can stay latched in ROM download mode.**
`Hard resetting via RTS pin` does not always clear it. It enumerates as Espressif
`303a:1001` and esptool talks to it happily while your app never runs. **Physically
unplug and replug USB.** Tell: with `ARDUINO_USB_MODE=0`, a running app must present
the board's own USB IDs — still seeing Espressif's means it is not executing.

**Do not trust the serial console for bring-up.** On this board HWCDC produced no
output at all, in either USB mode, via raw termios, `screen`, or `pio device monitor`.
The retained `stage` topic is what actually located the hang: publish a boot-stage
beacon at each step of `setup()` and read the last value that stuck.

**`WiFi.setHostname()` must be called before `WiFi.mode()`.** It only writes a static
buffer; the value is pushed to the interface inside `WiFiGenericClass::mode()`. Call
it after and you silently get the MAC-derived default. Worse, `WiFi.getHostname()`
reads that same buffer, so it reports the name you set even when the interface is
using the default — verify with mDNS instead.

**Pin PlatformIO's platform version.** `platform = espressif32` may resolve to a
release that requires a newer core than your stable PlatformIO. This project pins
`espressif32@6.9.0`.

## Repository layout

```
platformio.ini          build envs: feather (USB), ota (network), mintest (bisect)
src/main.cpp            firmware
src/secrets.h.example   template for the gitignored secrets.h
tools/parse_hid.py      HID report descriptor parser -> field map
tools/br1500ms2.hex     reference descriptor from a BR1500MS2
```

The `mintest` env builds a bare sketch with no libraries — useful for bisecting
whether a problem is your code or the board.

## License

MIT — see [LICENSE](LICENSE).
