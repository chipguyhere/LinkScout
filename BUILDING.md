# Building LinkScout from source

## Dependencies

Requires the Arduino IDE or `arduino-cli`, with:

| Dependency | Version used | Where |
| --- | --- | --- |
| esp32 core (Espressif) | 3.3.11 (ESP-IDF 5.5.5) | Boards Manager |
| `lvgl` | 9.5.0 | Library Manager |
| `chipguy_P486Panel_480display` | 1.0.0 | [`libraries/`](libraries) in this repository |
| `chipguy_P486Panel_touch` | 1.0.0 | [`libraries/`](libraries) in this repository |

The sketch folder carries its own `lv_conf.h`, so you don't need a global LVGL
configuration. In the Arduino IDE, copy the two folders in `libraries/` into your Arduino
`libraries` folder. With arduino-cli, point `--libraries` at them as below.

**Arduino IDE board settings:** Board **ESP32P4 Dev Module**, PSRAM **Enabled**, Flash Size
**32MB (256Mb)**, USB CDC On Boot **Enabled**.

**arduino-cli**, from the folder that contains your clone:

```sh
FQBN="esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=32M,CDCOnBoot=cdc"
arduino-cli compile --fqbn "$FQBN" --libraries LinkScout/libraries LinkScout
arduino-cli upload  --fqbn "$FQBN" -p PORT LinkScout
```

Upload through the panel's bottom USB-C port (the USB-to-UART bridge). Serial diagnostics
come out of `Serial0` at 115200 on that same port.

## Making a release

```sh
python3 make_release.py     # builds linkscout.bin
python3 make_repo_zip.py    # writes dist/LinkScout-YYYYMMDD.zip
```

`make_release.py` compiles the sketch and merges bootloader, partition table and app into
`linkscout.bin`, written at 0x0. The core's own `.merged.bin` is padded to the full 32 MB;
this one stops at the end of the app. The script also keeps your own directory names out of
the image: `assert()` and log messages otherwise embed full source paths, so it remaps them
at compile time and refuses to write an image that still contains your home directory.

`make_repo_zip.py` packages exactly what belongs in this repository: sketch, libraries,
docs, notices, `linkscout.bin`, `launchpad.toml`, `BUILD_INFO.txt` and `.gitignore`. Its
contents are what gets committed, and the zip itself is attached to the GitHub release as the
source matching that binary. It refuses to run if `linkscout.bin` is older than any source
file or contains a local path, and stops if any file looks like it holds a credential
(passwords, keys, certificates, Wi-Fi/MQTT settings).

The one-click installer reads [`launchpad.toml`](launchpad.toml) and `linkscout.bin` from
`main`, so committing a new `linkscout.bin` publishes it immediately.

## Source code and the LGPL

This repository is the complete source for the firmware: the sketch, plus the two
`chipguy_P486Panel_*` libraries in [`libraries/`](libraries), which aren't published
anywhere else. All the other dependencies are unmodified public releases at the versions
listed above and in `BUILD_INFO.txt`. With this source you can rebuild the firmware,
including against a modified Arduino-ESP32 core, as the core's LGPL-2.1 license allows.

## How it works

**Cresnet Monitor.** Cresnet's data pair is RS485 at 38400 baud, 8N1. The panel's bottom
board has an isolated, automatic-direction RS485 transceiver on UART1 (RX GPIO48, TX GPIO47).
The firmware never transmits, so the transceiver never drives the bus. Decoding comes from
watching a live bus: the master polls with `<ID> 00`, and a peripheral answers with a
length-prefixed message about a millisecond later. The parser runs in the UART RX interrupt
with microsecond timestamps; the full timing analysis is in the header comment of
[`cresnet_monitor.cpp`](cresnet_monitor.cpp).

**Switch Port Finder.** LLDP and CDP are link-layer frames that the IP stack drops, so after
`ETH.begin()` the sketch replaces the Ethernet driver's stack-input callback with a tap. The
tap decodes LLDP/CDP frames and passes every frame on to lwIP unchanged, so DHCP keeps
working. See [`ethsniff.cpp`](ethsniff.cpp) and [`neighbor.cpp`](neighbor.cpp).

**Reset button vs. power-on.** The ESP32-P4 can't tell a reset-button press from a power-up:
both report the same reset reason, and both clear its low-power memory. The board's audio
codec isn't wired to the reset line, so at each boot the firmware leaves a marker in two of
the codec's unused volume registers. If the marker is still there at the next boot, the reset
button was pressed; a power-off clears it. Crashes, watchdog resets and brownouts go back into
the saved tool, and if the codec can't be read the panel shows the menu, so it can never lock
you out of it. See [`boot_sentinel.cpp`](boot_sentinel.cpp).

**Installer.** The one-click link uses
[a fork of Espressif's ESP Launchpad](https://github.com/chipguyhere/esp-launchpad) whose only
change adds the panel's USB-serial chip, a WCH CH343 (`1a86:55d3`), to the port picker's
filter list. Espressif's version doesn't list it, so the port never appears there.
