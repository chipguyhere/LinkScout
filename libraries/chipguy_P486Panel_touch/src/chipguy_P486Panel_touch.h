/*
 * GT911 capacitive touch driver for ESP32-P4 P486 Panel
 */

#pragma once

#ifdef ARDUINO_ESP32P4_DEV

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"

#define TOUCH_MAX_POINTS  5
#define CHIPGUY_TOUCH_MAX_BUTTONS 1

// Forward declarations for internal types
typedef struct esp_lcd_touch_s esp_lcd_touch_t;
typedef esp_lcd_touch_t *esp_lcd_touch_handle_t;

typedef void (*esp_lcd_touch_interrupt_callback_t)(esp_lcd_touch_handle_t tp);

typedef struct {
    uint16_t x_max;
    uint16_t y_max;
    gpio_num_t rst_gpio_num;
    gpio_num_t int_gpio_num;
    struct {
        unsigned int reset: 1;
        unsigned int interrupt: 1;
    } levels;
    struct {
        unsigned int swap_xy: 1;
        unsigned int mirror_x: 1;
        unsigned int mirror_y: 1;
    } flags;
    void (*process_coordinates)(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
    esp_lcd_touch_interrupt_callback_t interrupt_callback;
    void *user_data;
    void *driver_data;
} esp_lcd_touch_config_t;

typedef struct {
    uint8_t points;
    struct {
        uint16_t x;
        uint16_t y;
        uint16_t strength;
    } coords[TOUCH_MAX_POINTS];
#if (CHIPGUY_TOUCH_MAX_BUTTONS > 0)
    uint8_t buttons;
    struct {
        uint8_t status;
    } button[CHIPGUY_TOUCH_MAX_BUTTONS];
#endif
    portMUX_TYPE lock;
} esp_lcd_touch_data_t;

struct esp_lcd_touch_s {
    esp_err_t (*enter_sleep)(esp_lcd_touch_handle_t tp);
    esp_err_t (*exit_sleep)(esp_lcd_touch_handle_t tp);
    esp_err_t (*read_data)(esp_lcd_touch_handle_t tp);
    bool (*get_xy)(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
#if (CHIPGUY_TOUCH_MAX_BUTTONS > 0)
    esp_err_t (*get_button_state)(esp_lcd_touch_handle_t tp, uint8_t n, uint8_t *state);
#endif
    esp_err_t (*set_swap_xy)(esp_lcd_touch_handle_t tp, bool swap);
    esp_err_t (*get_swap_xy)(esp_lcd_touch_handle_t tp, bool *swap);
    esp_err_t (*set_mirror_x)(esp_lcd_touch_handle_t tp, bool mirror);
    esp_err_t (*get_mirror_x)(esp_lcd_touch_handle_t tp, bool *mirror);
    esp_err_t (*set_mirror_y)(esp_lcd_touch_handle_t tp, bool mirror);
    esp_err_t (*get_mirror_y)(esp_lcd_touch_handle_t tp, bool *mirror);
    esp_err_t (*del)(esp_lcd_touch_handle_t tp);
    esp_lcd_touch_config_t config;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_data_t data;
};

// Public API
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp);
bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t tp);

// Simple C++ wrapper
class chipguy_P486Panel_touch {
public:
    // I2C pins default to GPIO 7 (SDA) and GPIO 8 (SCL)
    // Touch reset pin defaults to GPIO 23
    chipguy_P486Panel_touch(
        gpio_num_t sda = GPIO_NUM_7,
        gpio_num_t scl = GPIO_NUM_8,
        gpio_num_t rst = GPIO_NUM_23,
        uint16_t x_max = 720,
        uint16_t y_max = 720);

    bool begin();

    // Read touch data and return whether screen is touched.
    // x, y are set to the first touch point coordinates.
    bool read(uint16_t &x, uint16_t &y);

    // Get the raw handle for advanced use (e.g. direct LVGL indev callback)
    esp_lcd_touch_handle_t getHandle() { return _tp_handle; }

private:
    gpio_num_t _sda;
    gpio_num_t _scl;
    gpio_num_t _rst;
    uint16_t _x_max;
    uint16_t _y_max;
    esp_lcd_touch_handle_t _tp_handle = nullptr;
    i2c_master_bus_handle_t _i2c_bus = nullptr;
};

#endif // ARDUINO_ESP32P4_DEV
