/*
 * ESP32-P4 DSI Display driver with 480x480 LVGL buffer + PPA scaling to 720x720
 * For 720x720 MIPI DSI display
 */

#pragma once

#ifdef ARDUINO_ESP32P4_DEV

// This display driver allocates its framebuffers in PSRAM.  The board carries
// 32 MB of PSRAM (and 32 MB of flash), so there is ample room -- but it must
// be switched on: the Arduino IDE defines BOARD_HAS_PSRAM when PSRAM is
// enabled (Tools > PSRAM: "Enabled").
// Fail loudly at compile time rather than crashing at runtime if it's off.
#ifndef BOARD_HAS_PSRAM
#error "chipguy_P486Panel_480display requires PSRAM. Enable it via Tools > PSRAM: \"Enabled\" in the Arduino IDE."
#endif

// The Flash Size check that used to live here now sits in
// lv_setup_P486Panel_480.hpp, next to the include guard.  It belongs there because
// this header is compiled into the library's own .cpp as well, where a sketch's
// #define cannot reach it - putting the check in a header only ever included BY a
// sketch is what lets CHIPGUY_ALLOW_ANY_FLASH_SIZE be set the obvious way.

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"   // the DSI PHY supply
#include "driver/ppa.h"

class chipguy_P486Panel_480display {
public:
    chipguy_P486Panel_480display();

    // Initialize the display hardware
    bool begin();

    // Warm-boot reset promotion (on by default; see the long note in the .cpp).
    //
    // A soft reset (OTA, esp_restart(), panic, task/int watchdog) is CPU-only:
    // the panel and the DSI PHY stay powered, and the panel intermittently
    // comes up black.  When enabled, a boot that followed a soft reset brings
    // the display up normally and then, with the link still running, promotes
    // itself to a Super Watchdog reset (digital core + RTC domain); the boot
    // after that runs normally.  Measured: 20/20 black-panel recoveries.
    //
    // Costs one extra ~1.5-2 s boot cycle per soft reset and nothing on a cold
    // boot.  Guarded against boot loops by both the reset reason and an NVS
    // flag.  Runs in addition to the RESETn recovery pulse at the top of
    // begin().  Call before begin() to disable; that leaves only the pulse.
    void setWarmBootQuiesce(bool enable) { _quiesce_on_warm_boot = enable; }

    // Recovery path for a failed begin().  Holds the panel in hardware reset
    // for hold_ms -- far longer than the 20 ms assert used during normal
    // bring-up -- to let it fully quiesce, then restarts the ESP32 so the
    // whole init sequence runs again from a clean state.  Does not return.
    [[noreturn]] void resetAndRestart(uint32_t hold_ms = 10000);

    // Get 480x480 draw buffers (LVGL renders here, two for DIRECT mode)
    uint16_t *getDrawBuffer(uint8_t index);

    // Get 720x720 DMA framebuffer by index (0 or 1)
    uint16_t *getFramebuffer(uint8_t index);

    // Set which framebuffer the display hardware shows
    void setActiveFramebuffer(uint8_t index, bool wait_for_vsync = true);

    // PPA scale the given 480x480 draw buffer into specified 720x720 DMA fb, then activate it
    void scaleAndFlip(uint8_t draw_buf_index, uint8_t target_fb_index);

    // Wait for vsync (frame boundary)
    bool waitVsync(uint32_t timeout_ms = 50);

    // Get number of framebuffers
    uint8_t getNumFramebuffers();

    // Logical size (what LVGL sees)
    int16_t width() const { return 480; }
    int16_t height() const { return 480; }
    size_t framebufferSize() const { return (size_t)480 * 480 * sizeof(uint16_t); }

    // Native panel size
    int16_t nativeWidth() const { return 720; }
    int16_t nativeHeight() const { return 720; }

    // Set PPA rotation angle in degrees (0, 90, 180, 270 counter-clockwise)
    // Call before or after begin(); takes effect on next scaleAndFlip().
    void setRotation(int degrees);
    int  getRotation() const;

    // Backlight control (0 = off, 100 = full brightness)
    void setBacklight(int percentage);

    // Debug counters
    uint32_t getFrameCount();
    uint32_t getUnderrunCount();

private:
    void _sendInitCommands();
    // Drive RESETn low 100 ms, high 100 ms, release the pin (input, no pull),
    // then 100 ms of quiet -- the sequence measured to recover panels that stay
    // black even through an EN-pin reset.  Called first thing in begin(), before
    // any other hardware is touched.  The normal bring-up assert still runs
    // afterwards, unchanged; this only has to happen first.
    void _recoveryResetPulse();
    bool _shouldPromoteWarmBootReset();
    void _triggerDeepestReset();

    bool _quiesce_on_warm_boot = true;
    bool _promote_pending = false;

    esp_ldo_channel_handle_t _ldo_mipi_phy = nullptr;
    esp_lcd_dsi_bus_handle_t _mipi_dsi_bus = nullptr;

    esp_lcd_panel_handle_t _panel_handle = nullptr;
    esp_lcd_panel_io_handle_t _io_handle = nullptr;
    ppa_client_handle_t _ppa_client = nullptr;
    uint16_t *_draw_buffers[2] = {nullptr, nullptr};  // Two 480x480 PSRAM buffers
    ppa_srm_rotation_angle_t _rotation = PPA_SRM_ROTATION_ANGLE_0;
    int _rotation_degrees = 0;
};

#endif // ARDUINO_ESP32P4_DEV
