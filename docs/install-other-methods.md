# Other ways to install LinkScout

The [one-click installer](../README.md#installing-one-click) is the easiest way. Use these if
it didn't work for you.

All of them use the same cable and port: the panel's **bottom** USB-C port, the one
**furthest from the red light**, with a USB **data** cable.

The firmware is [`linkscout.bin`](../linkscout.bin) in this repository: one file, written at
address **0x0**. Writing it replaces everything on the panel.

## If the port doesn't appear or won't connect

- **Close anything else using the port**: a serial monitor, the Arduino IDE, or another
  browser tab with an installer open. Only one program can open a serial port at a time.
- **Force the panel into install mode.** Hold **BOOT**, press and release **RESET**, let go
  of BOOT, then try again.
- **Drivers.** Windows 10/11, macOS, Linux and ChromeOS need no driver. Windows 7 and 8 need
  WCH's CH343 driver. If a Mac won't connect, installing WCH's CH34x driver usually fixes it.

## Espressif's web flasher

Also runs in Chrome or Edge on a desktop computer. Download
[`linkscout.bin`](../linkscout.bin) first.

1. Open <https://espressif.github.io/esptool-js/>.
2. Click **Connect** and pick **USB Single Serial**.
3. Set **Flash Address** to `0x0`, choose `linkscout.bin`, and click **Program**.
4. Press the panel's **reset** button when it finishes.

## esptool (command line)

```sh
esptool --chip esp32p4 --port PORT write-flash 0x0 linkscout.bin
```

`PORT` is `COM3` or similar on Windows, `/dev/ttyACM0` on Linux, and `/dev/cu.usbmodem…` on
macOS (`/dev/cu.wchusbserial…` if WCH's driver is installed). Older esptool versions spell the
command `esptool.py … write_flash`.

Serial diagnostics are printed at 115200 baud on the same port.
