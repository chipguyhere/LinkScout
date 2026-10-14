/*
 * boot_sentinel.h - tells a reset-button press from a power-up, using the
 * audio codec's registers as memory that outlives the ESP32-P4.
 *
 * The P4 can't tell by itself: an EN-pin reset reports the same reset reason
 * as a power-up (CHIP_POWER_ON), and it clears LP SRAM as well, so the usual
 * RTC_NOINIT marker doesn't survive.  (Measured on this board: rst:0x1 and a
 * fresh LP SRAM after an RTS-driven EN pulse.)
 *
 * The board's ES8311 codec is a different matter.  It sits on the shared I2C
 * bus at 0x18, its supply has no switch, and it has no reset pin, so the EN
 * line doesn't reach it -- its registers hold their contents until the board
 * actually loses power, and come back as datasheet defaults afterwards.
 * Nothing in this firmware uses audio, so two of its volume registers make a
 * sentinel: present means "this board has already booted since power-up".
 */

#pragma once

enum SentinelState {
    SENTINEL_ABSENT,       // codec registers at their power-up defaults
    SENTINEL_PRESENT,      // our pattern was already there: warm reset
    SENTINEL_UNAVAILABLE,  // codec didn't answer, or the write didn't stick
};

// Reads the sentinel, then sets it for next time.  Needs the I2C bus that
// chipguy_P486Panel_touch opens on port 0, so call it after lv_setup.begin().
SentinelState boot_sentinel_check_and_arm();
