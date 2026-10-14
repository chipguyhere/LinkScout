/*******************************************************************************
 * ESP32-P4 P486Panel LVGL Bouncing Balls
 *
 * For Waveshare ESP32-P4-86-Panel-ETH-2RO
 *
 * A resolution-independent demo: 1..20 circular balls bounce under gravity,
 * each a random color, re-launching to a random height on every floor bounce.
 * Two sliders at the bottom select the number of balls and the speed.
 *
 * The whole UI is laid out from the live display resolution at runtime (see
 * ui.cpp), so the identical ui.cpp / ui.h drop unchanged onto any board whose
 * lv_setup.hpp follows the same shape, in any orientation.
 *
 * LVGL renders at 480x480; the PPA hardware scales it to the 720x720 panel.
 *
 * Mounting the device rotated?  Uncomment the display.setRotation() line in
 * setup() below.  Touch is transformed to match automatically.
 *
 * Requires the following libraries:
 *   - chipguy_P486Panel_480display  (display driver)
 *   - chipguy_P486Panel_touch    (capacitive touch driver)
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
 * Copyright (c) 2025 chipguyhere
 * MIT License
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
// It's important for the config and library versions to be compatible.
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// The bouncing-balls UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    // Note: Serial0 is always UART0, which is the BOTTOM USB-C port (the CH340
    // bridge) -- regardless of the "USB CDC On Boot" setting.  That setting only
    // decides what plain `Serial` means: the native-USB device on the top port
    // when Enabled, or an alias for Serial0 when Disabled.  Leaving it Disabled
    // saves ~50 KB of internal DRAM.
    Serial0.begin(115200);

    // Mounted sideways?  Uncomment to rotate the UI 90 degrees so it reads
    // upright, with the USB ports and buttons along the bottom edge where they
    // are least visible.  Accepts 0, 90, 180 or 270.
    // display.setRotation(90);

    // Initialize display, touch, and LVGL (touch transform is automatic).
    // The UI adapts to whatever resolution the display reports.
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

    // Build the bouncing-balls screen.
    ui_init();
}

void loop() {
    // Give loop control to LVGL.
    lv_timer_handler();
    delay(5);
}
