/*
 * GT911 capacitive touch driver for ESP32-P4 P486 Panel
 */

#ifdef ARDUINO_ESP32P4_DEV

#include "chipguy_P486Panel_touch.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG_TP = "TP";
static const char *TAG_GT911 = "GT911";

/*******************************************************************************
 * Touch API implementation (from touch.cpp)
 ******************************************************************************/

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp) {
    assert(tp != NULL);
    assert(tp->read_data != NULL);
    return tp->read_data(tp);
}

bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {
    bool touched = false;
    assert(tp != NULL);
    assert(x != NULL);
    assert(y != NULL);
    assert(tp->get_xy != NULL);

    touched = tp->get_xy(tp, x, y, strength, point_num, max_point_num);
    if (!touched) return false;

    if (tp->config.process_coordinates != NULL) {
        tp->config.process_coordinates(tp, x, y, strength, point_num, max_point_num);
    }

    bool sw_adj_needed = ((tp->config.flags.mirror_x && (tp->set_mirror_x == NULL)) ||
                          (tp->config.flags.mirror_y && (tp->set_mirror_y == NULL)) ||
                          (tp->config.flags.swap_xy && (tp->set_swap_xy == NULL)));

    for (int i = 0; (sw_adj_needed && i < *point_num); i++) {
        if (tp->config.flags.mirror_x && tp->set_mirror_x == NULL)
            x[i] = tp->config.x_max - x[i];
        if (tp->config.flags.mirror_y && tp->set_mirror_y == NULL)
            y[i] = tp->config.y_max - y[i];
        if (tp->config.flags.swap_xy && tp->set_swap_xy == NULL) {
            uint16_t tmp = x[i]; x[i] = y[i]; y[i] = tmp;
        }
    }
    return touched;
}

esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t tp) {
    assert(tp != NULL);
    if (tp->del != NULL) return tp->del(tp);
    return ESP_OK;
}

/*******************************************************************************
 * GT911 driver implementation (from gt911.cpp)
 ******************************************************************************/

#define GT911_I2C_ADDR          0x5D
#define GT911_READ_XY_REG       0x814E
#define GT911_CONFIG_REG        0x8047
#define GT911_PRODUCT_ID_REG    0x8140
#define GT911_ENTER_SLEEP       0x8040
#define GT911_READ_KEY_REG      0x8093
#define GT911_MAX_BUTTONS       4

static esp_err_t gt911_read_data(esp_lcd_touch_handle_t tp);
static bool gt911_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
#if (CHIPGUY_TOUCH_MAX_BUTTONS > 0)
static esp_err_t gt911_get_button_state(esp_lcd_touch_handle_t tp, uint8_t n, uint8_t *state);
#endif
static esp_err_t gt911_del(esp_lcd_touch_handle_t tp);
static esp_err_t gt911_enter_sleep(esp_lcd_touch_handle_t tp);
static esp_err_t gt911_exit_sleep(esp_lcd_touch_handle_t tp);
static esp_err_t gt911_i2c_read(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);
static esp_err_t gt911_i2c_write(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t data);
static esp_err_t gt911_reset(esp_lcd_touch_handle_t tp);
static esp_err_t gt911_read_cfg(esp_lcd_touch_handle_t tp);

static esp_err_t gt911_new_i2c(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch) {
    esp_err_t ret = ESP_OK;
    if (!io || !config || !out_touch) return ESP_ERR_INVALID_ARG;

    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    if (!tp) return ESP_ERR_NO_MEM;

    tp->io = io;
    tp->read_data = gt911_read_data;
    tp->get_xy = gt911_get_xy;
#if (CHIPGUY_TOUCH_MAX_BUTTONS > 0)
    tp->get_button_state = gt911_get_button_state;
#endif
    tp->del = gt911_del;
    tp->enter_sleep = gt911_enter_sleep;
    tp->exit_sleep = gt911_exit_sleep;
    tp->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&tp->config, config, sizeof(esp_lcd_touch_config_t));

    if (config->rst_gpio_num != GPIO_NUM_NC) {
        gpio_config_t rst_gpio_config = {
            .pin_bit_mask = BIT64(config->rst_gpio_num),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&rst_gpio_config);
        if (ret != ESP_OK) { heap_caps_free(tp); return ret; }
    }

    if (config->int_gpio_num != GPIO_NUM_NC) {
        gpio_config_t int_gpio_config = {
            .pin_bit_mask = BIT64(config->int_gpio_num),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = config->levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
        };
        ret = gpio_config(&int_gpio_config);
        if (ret != ESP_OK) { heap_caps_free(tp); return ret; }
    }

    ret = gt911_reset(tp);
    if (ret != ESP_OK) { heap_caps_free(tp); return ret; }

    ret = gt911_read_cfg(tp);
    if (ret != ESP_OK) { heap_caps_free(tp); return ret; }

    *out_touch = tp;
    return ESP_OK;
}

static esp_err_t gt911_enter_sleep(esp_lcd_touch_handle_t tp) {
    return gt911_i2c_write(tp, GT911_ENTER_SLEEP, 0x05);
}

static esp_err_t gt911_exit_sleep(esp_lcd_touch_handle_t tp) {
    esp_err_t ret;
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config_high = {
            .pin_bit_mask = BIT64(tp->config.int_gpio_num),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&int_gpio_config_high);
        ESP_RETURN_ON_ERROR(ret, TAG_GT911, "High GPIO config failed");
        gpio_set_level(tp->config.int_gpio_num, 1);
        vTaskDelay(pdMS_TO_TICKS(5));

        const gpio_config_t int_gpio_config_float = {
            .pin_bit_mask = BIT64(tp->config.int_gpio_num),
            .mode = GPIO_MODE_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&int_gpio_config_float);
        ESP_RETURN_ON_ERROR(ret, TAG_GT911, "Float GPIO config failed");
    }
    return ESP_OK;
}

static esp_err_t gt911_read_data(esp_lcd_touch_handle_t tp) {
    esp_err_t err;
    uint8_t buf[41];
    uint8_t touch_cnt = 0;
    uint8_t clear = 0;
    size_t i = 0;

    assert(tp != NULL);

    err = gt911_i2c_read(tp, GT911_READ_XY_REG, buf, 1);
    ESP_RETURN_ON_ERROR(err, TAG_GT911, "I2C read error!");

    if ((buf[0] & 0x80) == 0x00) {
        gt911_i2c_write(tp, GT911_READ_XY_REG, clear);
#if (CHIPGUY_TOUCH_MAX_BUTTONS > 0)
    } else if ((buf[0] & 0x10) == 0x10) {
        uint8_t key_max = ((GT911_MAX_BUTTONS < CHIPGUY_TOUCH_MAX_BUTTONS) ?
                           GT911_MAX_BUTTONS : CHIPGUY_TOUCH_MAX_BUTTONS);
        err = gt911_i2c_read(tp, GT911_READ_KEY_REG, &buf[0], key_max);
        ESP_RETURN_ON_ERROR(err, TAG_GT911, "I2C read error!");
        gt911_i2c_write(tp, GT911_READ_XY_REG, clear);
        ESP_RETURN_ON_ERROR(err, TAG_GT911, "I2C write error!");

        portENTER_CRITICAL(&tp->data.lock);
        tp->data.buttons = key_max;
        for (i = 0; i < key_max; i++) {
            tp->data.button[i].status = buf[0] ? 1 : 0;
        }
        portEXIT_CRITICAL(&tp->data.lock);
#endif
    } else if ((buf[0] & 0x80) == 0x80) {
#if (CHIPGUY_TOUCH_MAX_BUTTONS > 0)
        portENTER_CRITICAL(&tp->data.lock);
        for (i = 0; i < CHIPGUY_TOUCH_MAX_BUTTONS; i++) {
            tp->data.button[i].status = 0;
        }
        portEXIT_CRITICAL(&tp->data.lock);
#endif
        touch_cnt = buf[0] & 0x0f;
        if (touch_cnt > 5 || touch_cnt == 0) {
            gt911_i2c_write(tp, GT911_READ_XY_REG, clear);
            return ESP_OK;
        }

        err = gt911_i2c_read(tp, GT911_READ_XY_REG + 1, &buf[1], touch_cnt * 8);
        ESP_RETURN_ON_ERROR(err, TAG_GT911, "I2C read error!");

        err = gt911_i2c_write(tp, GT911_READ_XY_REG, clear);
        ESP_RETURN_ON_ERROR(err, TAG_GT911, "I2C read error!");

        portENTER_CRITICAL(&tp->data.lock);
        touch_cnt = (touch_cnt > TOUCH_MAX_POINTS ? TOUCH_MAX_POINTS : touch_cnt);
        tp->data.points = touch_cnt;

        for (i = 0; i < touch_cnt; i++) {
            tp->data.coords[i].x = ((uint16_t)buf[(i * 8) + 3] << 8) + buf[(i * 8) + 2];
            tp->data.coords[i].y = (((uint16_t)buf[(i * 8) + 5] << 8) + buf[(i * 8) + 4]);
            tp->data.coords[i].strength = (((uint16_t)buf[(i * 8) + 7] << 8) + buf[(i * 8) + 6]);
        }
        portEXIT_CRITICAL(&tp->data.lock);
    }
    return ESP_OK;
}

static bool gt911_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {
    assert(tp != NULL);
    assert(x != NULL && y != NULL && point_num != NULL && max_point_num > 0);

    portENTER_CRITICAL(&tp->data.lock);
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) strength[i] = tp->data.coords[i].strength;
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
    return (*point_num > 0);
}

#if (CHIPGUY_TOUCH_MAX_BUTTONS > 0)
static esp_err_t gt911_get_button_state(esp_lcd_touch_handle_t tp, uint8_t n, uint8_t *state) {
    assert(tp != NULL && state != NULL);
    *state = 0;
    portENTER_CRITICAL(&tp->data.lock);
    if (n > tp->data.buttons) {
        portEXIT_CRITICAL(&tp->data.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *state = tp->data.button[n].status;
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}
#endif

static esp_err_t gt911_del(esp_lcd_touch_handle_t tp) {
    assert(tp != NULL);
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback)
            gpio_isr_handler_remove(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC)
        gpio_reset_pin(tp->config.rst_gpio_num);
    free(tp);
    return ESP_OK;
}

static esp_err_t gt911_reset(esp_lcd_touch_handle_t tp) {
    assert(tp != NULL);
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG_GT911, "GPIO set level error!");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG_GT911, "GPIO set level error!");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static esp_err_t gt911_read_cfg(esp_lcd_touch_handle_t tp) {
    uint8_t buf[4];
    assert(tp != NULL);
    ESP_RETURN_ON_ERROR(gt911_i2c_read(tp, GT911_PRODUCT_ID_REG, &buf[0], 3), TAG_GT911, "GT911 read error!");
    ESP_RETURN_ON_ERROR(gt911_i2c_read(tp, GT911_CONFIG_REG, &buf[3], 1), TAG_GT911, "GT911 read error!");
    ESP_LOGI(TAG_GT911, "TouchPad_ID:0x%02x,0x%02x,0x%02x", buf[0], buf[1], buf[2]);
    ESP_LOGI(TAG_GT911, "TouchPad_Config_Version:%d", buf[3]);
    return ESP_OK;
}

static esp_err_t gt911_i2c_read(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len) {
    assert(tp != NULL && data != NULL);
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t gt911_i2c_write(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t data) {
    assert(tp != NULL);
    uint8_t data_array[1] = {data};
    return esp_lcd_panel_io_tx_param(tp->io, reg, data_array, 1);
}

/*******************************************************************************
 * C++ wrapper implementation
 ******************************************************************************/

chipguy_P486Panel_touch::chipguy_P486Panel_touch(gpio_num_t sda, gpio_num_t scl, gpio_num_t rst, uint16_t x_max, uint16_t y_max)
    : _sda(sda), _scl(scl), _rst(rst), _x_max(x_max), _y_max(y_max) {}

bool chipguy_P486Panel_touch::begin() {
    // Assert GT911 reset (active low) before bringing up the I2C master.
    // After a watchdog reset the GT911 has not been power-cycled and may
    // still be clamping SDA from a half-completed transaction; holding
    // RESETn low first guarantees it releases the bus before I2C init.
    if (_rst != GPIO_NUM_NC) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = 1ULL << _rst,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);
        gpio_set_level(_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Initialize I2C bus
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = _sda,
        .scl_io_num = _scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    if (i2c_new_master_bus(&i2c_bus_config, &_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG_GT911, "I2C bus init failed");
        return false;
    }

    delay(10);
    delay(200);

    // Create panel IO for GT911
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = GT911_I2C_ADDR,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = {
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 400 * 1000,
    };
    if (esp_lcd_new_panel_io_i2c(_i2c_bus, &tp_io_config, &tp_io_handle) != ESP_OK) {
        ESP_LOGE(TAG_GT911, "Panel IO init failed");
        return false;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = _x_max,
        .y_max = _y_max,
        .rst_gpio_num = _rst,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };

    // GT911 may not respond after a soft reboot due to stale controller state.
    // Keep retrying until it responds.
    for (int attempt = 1; attempt <= 20; attempt++) {
        if (gt911_new_i2c(tp_io_handle, &tp_cfg, &_tp_handle) == ESP_OK) {
            return true;
        }
        _tp_handle = nullptr;
        ESP_LOGW(TAG_GT911, "GT911 init attempt %d failed, retrying...", attempt);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ESP_LOGE(TAG_GT911, "GT911 init failed after 20 attempts");
    return false;
}

bool chipguy_P486Panel_touch::read(uint16_t &x, uint16_t &y) {
    if (!_tp_handle) return false;

    uint16_t tx[TOUCH_MAX_POINTS], ty[TOUCH_MAX_POINTS];
    uint16_t strength[TOUCH_MAX_POINTS];
    uint8_t cnt = 0;

    esp_lcd_touch_read_data(_tp_handle);
    bool pressed = esp_lcd_touch_get_coordinates(_tp_handle, tx, ty, strength, &cnt, TOUCH_MAX_POINTS);

    if (pressed && cnt > 0) {
        x = tx[0];
        y = ty[0];
        return true;
    }
    return false;
}

#endif // ARDUINO_ESP32P4_DEV
