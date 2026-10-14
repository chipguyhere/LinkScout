/*
 * lv_setup.hpp - Connects LVGL to the P486 Panel 480display and touch hardware.
 * LVGL renders at 480x480, PPA scales to 720x720 for display output.
 * Include this file after lvgl.h in your sketch.
 */

#pragma once

#include <chipguy_P486Panel_480display.h>
#include <chipguy_P486Panel_touch.h>
#include "esp_cache.h"

static chipguy_P486Panel_480display display;

class lv_setup_class {
public:
    // Initializes display, touch input, and LVGL.
    void begin() {
        if (!display.begin()) {
            // Don't strand the board in a dead loop: hold the panel in reset
            // for 10 s and reboot, so an init failure retries by itself.
            Serial0.println("Display init failed! Resetting panel and rebooting in 10s...");
            Serial0.flush();
            display.resetAndRestart(10000);  // does not return
        }
        Serial0.println("Display initialized");

        _touchscreen.begin();

        _drawBuf0 = (uint8_t *)display.getDrawBuffer(0);
        _drawBuf1 = (uint8_t *)display.getDrawBuffer(1);
        _drawBufSize = display.framebufferSize();

        lv_init();

        static lv_disp_t *disp = lv_display_create(display.width(), display.height());
        lv_display_set_flush_cb(disp, _disp_flush);
        lv_display_set_buffers(disp, _drawBuf0, _drawBuf1, _drawBufSize, LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_user_data(disp, this);

        static lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, _touchpad_read);
        lv_indev_set_user_data(indev, this);

        lv_tick_set_cb(_tick_get);
    }

private:
    chipguy_P486Panel_touch _touchscreen;
    uint8_t *_drawBuf0 = nullptr;
    uint8_t *_drawBuf1 = nullptr;
    size_t _drawBufSize = 0;
    uint8_t _target_fb_index = 0;

    static uint32_t _tick_get(void) { return millis(); }

    static void _disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        auto *self = (lv_setup_class *)lv_display_get_user_data(disp);
        if (lv_display_flush_is_last(disp)) {
            // Determine which draw buffer LVGL just finished rendering into
            uint8_t draw_buf_index = (pixelmap == self->_drawBuf0) ? 0 : 1;

            // Scale that 480x480 buffer into the next 720x720 DMA framebuffer
            display.scaleAndFlip(draw_buf_index, self->_target_fb_index);
            self->_target_fb_index ^= 1;
        }
        lv_display_flush_ready(disp);
    }

    static void _touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
        auto *self = (lv_setup_class *)lv_indev_get_user_data(indev);
        esp_lcd_touch_handle_t tp = self->_touchscreen.getHandle();
        if (!tp) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        uint16_t x[TOUCH_MAX_POINTS], y[TOUCH_MAX_POINTS];
        uint16_t strength[TOUCH_MAX_POINTS];
        uint8_t cnt = 0;

        esp_lcd_touch_read_data(tp);
        bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &cnt, TOUCH_MAX_POINTS);

        if (pressed && cnt > 0) {
            // Scale touch from native 720 to logical 480 (multiply by 2/3)
            data->point.x = (int32_t)x[0] * 2 / 3;
            data->point.y = (int32_t)y[0] * 2 / 3;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
};

static lv_setup_class lv_setup;
