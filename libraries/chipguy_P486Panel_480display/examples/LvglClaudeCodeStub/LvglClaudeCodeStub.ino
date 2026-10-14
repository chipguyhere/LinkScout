/*******************************************************************************
 * ESP32-P4 P486Panel LVGL Claude Code Stub (480x480 + PPA scaling)
 *
 * For Waveshare ESP32-P4-86-Panel-ETH-2RO
 *
 * A minimal starting point for building an LVGL application from scratch on
 * this hardware.  The UI lives in a tiny ui.h / ui.cpp pair in the sketch
 * folder that draws a simple "Hello, world!" screen.
 *
 * LVGL renders at 480x480 and the PPA hardware scales the image to the 720x720
 * panel.  This maximizes compatibility with other sketches that target this
 * form factor at 480x480.
 *
 * Mounting the device rotated?  Uncomment the display.setRotation() line in
 * setup() below.  Touch is transformed to match automatically.
 *
 * The intent is to hand this to Claude Code (or any developer) as a clean
 * scaffold: the display, touch, and LVGL plumbing is already wired up, so you
 * can focus on writing your own UI in ui.cpp.  Just replace the contents of
 * ui_init() with your own widgets and grow from there.  Coordinates are in the
 * logical 480x480 space.
 *
 * Demonstrates touchscreen, LVGL and a hand-written UI.
 *
 * Compatible with LVGL 9; the bundled lv_conf.h is based on LVGL 9.3.  If your
 * installed LVGL differs, the API is generally close enough to adapt.
 *
 * Requires the following libraries:
 *   - chipguy_P486Panel_480display  (display driver with PPA scaling)
 *   - chipguy_P486Panel_touch       (capacitive touch driver)
 *   - lvgl 9
 *
 * Board hardware: 32 MB flash, 32 MB PSRAM
 *
 * Arduino IDE Board Settings:
 *   - Board: ESP32P4 Dev Module
 *   - PSRAM: Enabled
 *   - Flash Size: 32 MegaBytes (256 megabits)
 *   - USB CDC On Boot: Disabled  <- bottom USB-C port (CH340 into UART0)
 *                                  Enabled for the top port (native USB);
 *                                  Enabled costs ~50 KB of internal DRAM.
 *                                  Serial0 is always UART0 = bottom port,
 *                                  whichever way this is set.
 *
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
// It's important for the config and library versions to be compatible.
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// Fonts
// The Montserrat font is built into LVGL in multiple sizes, each size takes up memory.
// We enable the 14 point font as a default font.
#define LV_FONT_MONTSERRAT_14 1
// need other sizes?  You'll know if you see compiler errors after trying to
// use them.  Just edit lv_conf.h where FONTs are turned off (0) and turn them on (1)

// The UI for this example lives in ui.h / ui.cpp in this sketch folder.
// It is a hand-written "Hello, world!" screen meant as a starting point.
// Build your own application by editing ui_init() in ui.cpp.
#include "ui.h"

void setup() {
    Serial0.begin(115200);

    // Mounted sideways?  Uncomment to rotate the UI 90 degrees so it reads
    // upright, with the USB ports and buttons along the bottom edge where they
    // are least visible.  Accepts 0, 90, 180 or 270.
    // display.setRotation(90);

    // Initialize display, touch, and LVGL (touch transform is automatic)
    //
    // NOTE: this call may REBOOT the board once, and that is normal.  Following
    // a warm restart -- ESP.restart(), an OTA reboot, a panic, or a software
    // watchdog reset -- the driver deliberately promotes the soft reset to a
    // deeper system+RTC watchdog reset, to force the LCD to fully reset along
    // with the chip.  A board errata otherwise leaves the panel persistently
    // BLACK after a soft reset: backlight on and firmware running, just no
    // picture, until something resets the RTC domain too.
    //
    // It costs one extra ~1.5 s boot cycle per warm restart, never fires on a
    // cold boot, and is guarded against boot loops.  The catch for your code:
    // whatever setup() did before this line runs AGAIN after the reboot, so
    // don't put must-happen-once side effects above it.
    //
    // See lv_setup.hpp for the details; display.setWarmBootQuiesce(false),
    // called before this line, opts out.
    lv_setup.begin();
    Serial0.printf("LVGL initialized with %dx%d touchscreen (rotated %d degrees)\n",
                   display.width(), display.height(), display.getRotation());

    // Start the application's own setup
    ui_init();
}


void loop() {
    // Give loop control to LVGL objects created by the application
    lv_timer_handler();
    delay(5);
}
