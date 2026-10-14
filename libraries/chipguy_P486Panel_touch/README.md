# chipguy_P486Panel_touch

The touch-screen driver for the **Waveshare ESP32-P4-86-Panel-ETH-2RO** (and the front
board on its own, the "ESP32-P4-WIFI6-Touch-LCD-4B"). It reads where a finger is on the
4" 720×720 screen.

**You usually don't call this library yourself.** The display libraries'
`lv_setup.begin()` starts it and connects it to LVGL, so touches arrive in your LVGL app
as ordinary clicks and drags. Install it, and the examples in
[`chipguy_P486Panel_display`](../chipguy_P486Panel_display) or
[`chipguy_P486Panel_480display`](../chipguy_P486Panel_480display) handle the rest.

---

## Installation

This library is distributed by downloading the code as a ZIP from GitHub — there is no
Arduino Library Manager entry. Click **Code ▸ Download ZIP**, unzip it, and move the
folder into your Arduino **libraries** folder (`Documents/Arduino/libraries/`). Restart
the Arduino IDE.

## Using it directly

Only needed if you are not using `lv_setup`:

```cpp
#include <chipguy_P486Panel_touch.h>

chipguy_P486Panel_touch touch;

void setup() {
    Serial0.begin(115200);
    touch.begin();                    // returns false if the touch chip didn't answer
}

void loop() {
    uint16_t x, y;
    if (touch.read(x, y)) {           // true while a finger is down
        Serial0.printf("%u, %u\n", x, y);
    }
    delay(20);
}
```

`x` and `y` run from 0 to 719, with 0,0 at the top-left of the screen in its normal
orientation. `read()` reports the first finger only.

## Things worth knowing

- **It shares a connection with the sound chips.** The touch chip talks over the board's
  I²C bus, which the audio chips (and a camera, if fitted) also use. This library opens
  that bus. Other code that wants the bus has to join it rather than open its own — see
  [README-board.md](../chipguy_P486Panel_display/README-board.md#the-i2c-bus).
- **It can't wake a sleeping board.** The touch chip's "I've been touched" signal isn't
  wired to the processor, so the library checks for touches by asking regularly.
- **Starting up can take a moment.** After a software restart the touch chip sometimes
  needs a few tries to answer, and the library retries for up to about five seconds.

## Where to go next

| File | Who it's for |
|---|---|
| [README-board.md — The I2C bus](../chipguy_P486Panel_display/README-board.md#the-i2c-bus) | Developers: addresses on the shared bus and how to add a device to it. |
| [README-engineering.md — Touch](../chipguy_P486Panel_display/README-engineering.md#touch-gt911) | Engineers: the GT911, its address, the reset-before-I²C order, why there's no interrupt. |

Those files live in the display library, [`chipguy_P486Panel_display`](../chipguy_P486Panel_display),
which holds the board documentation for this whole family.
