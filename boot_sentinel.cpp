/*
 * boot_sentinel.cpp - see boot_sentinel.h.
 *
 * Register choice: ADC volume (0x17) and DAC volume (0x32).  Both are plain
 * 8-bit read/write registers that default to 0x00 at power-up, and with the
 * ADC and DAC still powered down (as they are -- nothing here wakes the codec)
 * their values have no effect.  The speaker amplifier stays off regardless:
 * its enable is GPIO53, which nothing here drives.
 *
 * Two registers holding a value and its complement, so a codec that happens to
 * power up with one of them non-zero can't pass for the sentinel.
 */

#include <Arduino.h>
#include "driver/i2c_master.h"
#include "boot_sentinel.h"

static constexpr uint8_t  ES8311_ADDR = 0x18;
static constexpr uint8_t  REG_A       = 0x17;   // ADC volume
static constexpr uint8_t  REG_B       = 0x32;   // DAC volume
static constexpr uint8_t  PATTERN_A   = 0x4C;   // 'L'
static constexpr uint8_t  PATTERN_B   = (uint8_t)~PATTERN_A;
static constexpr int      TIMEOUT_MS  = 50;

static bool read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, TIMEOUT_MS) == ESP_OK;
}

static bool write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), TIMEOUT_MS) == ESP_OK;
}

SentinelState boot_sentinel_check_and_arm() {
    i2c_master_bus_handle_t bus;
    if (i2c_master_get_bus_handle(I2C_NUM_0, &bus) != ESP_OK) {
        Serial0.println("boot sentinel: I2C bus not open");
        return SENTINEL_UNAVAILABLE;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8311_ADDR,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        Serial0.println("boot sentinel: could not attach ES8311");
        return SENTINEL_UNAVAILABLE;
    }

    SentinelState result = SENTINEL_UNAVAILABLE;
    uint8_t a = 0, b = 0;
    if (read_reg(dev, REG_A, &a) && read_reg(dev, REG_B, &b)) {
        bool present = (a == PATTERN_A && b == PATTERN_B);

        // Arm for the next reset, and read back: a codec that ACKs but drops
        // the write would otherwise report "absent" on every boot.
        uint8_t ra = 0, rb = 0;
        if (write_reg(dev, REG_A, PATTERN_A) && write_reg(dev, REG_B, PATTERN_B) &&
            read_reg(dev, REG_A, &ra) && read_reg(dev, REG_B, &rb) &&
            ra == PATTERN_A && rb == PATTERN_B) {
            result = present ? SENTINEL_PRESENT : SENTINEL_ABSENT;
        }
        Serial0.printf("boot sentinel: read %02x/%02x, armed %02x/%02x\n", a, b, ra, rb);
    } else {
        Serial0.println("boot sentinel: ES8311 did not answer");
    }

    i2c_master_bus_rm_device(dev);
    return result;
}
