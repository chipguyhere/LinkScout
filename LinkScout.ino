/*******************************************************************************
 * LinkScout - a two-tool field kit on the Waveshare ESP32-P4-86-Panel
 *
 * On boot the panel shows a menu with two large buttons; the one tapped
 * decides what the unit does until the next power cycle:
 *
 *   Cresnet Monitor     - the passive Cresnet bus monitor from
 *                         6115-cresnet-86-monitor: a 16x16 grid of every
 *                         Cresnet ID, coloured by whether the master polls it
 *                         and whether it answers.  RS485 transceiver on UART1,
 *                         RX=GPIO48, TX=GPIO47 (held idle).
 *   Switch Port Finder  - lldp_display: the LLDP/CDP neighbour on the
 *                         Ethernet port, plus the DHCP lease and link state.
 *
 * There's deliberately no way back to the menu.  Each tool claims hardware
 * (the UART ISR, the Ethernet driver's input path) that it never gives back,
 * and a reboot is the honest way to switch.
 *
 * "Always start this tool": a checkbox on the menu saves the next tool chosen
 * to NVS, and from then on a power-on goes straight into it.  Pressing the
 * reset button brings the menu back, where the box can be cleared.  Telling
 * those two apart takes some care on the P4 -- see "Reset vs power-on" below.
 *
 * Networking is limited to what the Switch Port Finder needs: Ethernet with
 * DHCP, receive-side only.  The MQTT/NetClient/OTA side of the Cresnet monitor
 * is not carried over, so no broker name, credentials, CA certificate or
 * per-unit MAC table are in this sketch or the binary built from it.
 *
 * A dim "Info" label in the lower-right corner of the menu pops up the build
 * date, author credit and open-source notices.
 *
 * Requires the following libraries:
 *   - chipguy_P486Panel_480display  (display driver with PPA scaling)
 *   - chipguy_P486Panel_touch       (capacitive touch driver)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings:
 *   - Board: ESP32P4 Dev Module
 *   - PSRAM: Enabled
 *   - Flash Size: 32 MegaBytes (256 megabits)
 *   - USB CDC On Boot: Enabled
 *
 * Serial diagnostics on Serial0 (the CH340 bridge) at 115200.
 *
 ******************************************************************************/

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

#include <ETH.h>
#include <Network.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>

#include "cresnet_monitor.h"
#include "cresnet_grid.hpp"
#include "neighbor.h"
#include "ethsniff.h"
#include "ui.h"
#include "github_mark.h"
#include "boot_sentinel.h"

enum Tool : uint8_t {
    TOOL_NONE = 0,
    TOOL_CRESNET,
    TOOL_LLDP,
};

// Set by a menu button, acted on in loop().  Starting a tool replaces the
// menu screen, and deleting a screen from inside its own button's event
// callback is asking for trouble -- so the callback only records the choice.
static volatile Tool s_pendingTool = TOOL_NONE;
static Tool          s_activeTool  = TOOL_NONE;

// ---------------------------------------------------- reset vs power-on ----
// The P4 reports a reset-button (EN pin) reset as ESP_RST_POWERON, the same as
// a real power-up, and wipes its own LP SRAM either way, so the chip can't tell
// the two apart by itself.  The audio codec can: it isn't on the EN line, so a
// sentinel written to it survives the reset button but not a power-off.  See
// boot_sentinel.cpp.

// The remembered tool lives in NVS so it survives power-off.
static const char *PREFS_NS  = "linkscout";
static const char *PREFS_KEY = "autotool";
static bool s_rememberChoice = false;   // mirrors the menu checkbox

// ------------------------------------------------------------- palette ----
// Shared with the Switch Port Finder's screen (ui.cpp) so the menu doesn't
// look like a different product.

#define MENU_SCREEN    lv_color_hex(0x0d1117)
#define MENU_CARD      lv_color_hex(0x161b22)
#define MENU_BORDER    lv_color_hex(0x30363d)
#define MENU_CAPTION   lv_color_hex(0x6b7785)
#define MENU_VALUE     lv_color_hex(0xe6edf3)
#define MENU_INFO      lv_color_hex(0x3a424c)   // the low-contrast Info label

#define BTN_CRESNET    lv_color_hex(0x1a7f37)
#define BTN_LLDP       lv_color_hex(0x1f6feb)

static lv_obj_t *s_infoPopup = nullptr;

// ------------------------------------------------------- saved choice ----

static Tool load_saved_tool() {
    Preferences prefs;
    if (!prefs.begin(PREFS_NS, true)) return TOOL_NONE;   // namespace not created yet
    uint8_t t = prefs.getUChar(PREFS_KEY, TOOL_NONE);
    prefs.end();
    return (t == TOOL_CRESNET || t == TOOL_LLDP) ? (Tool)t : TOOL_NONE;
}

static void save_tool(Tool tool) {
    Preferences prefs;
    if (!prefs.begin(PREFS_NS, false)) return;
    if (tool == TOOL_NONE) prefs.remove(PREFS_KEY);
    else                   prefs.putUChar(PREFS_KEY, tool);
    prefs.end();
}

// ---------------------------------------------------------------- menu ----

static void on_tool_button(lv_event_t *e) {
    if (s_pendingTool != TOOL_NONE) return;   // a second tap while starting
    Tool tool = (Tool)(uintptr_t)lv_event_get_user_data(e);
    if (s_rememberChoice) save_tool(tool);
    s_pendingTool = tool;
}

// Ticking the box only arms it -- there's nothing to save until a tool is
// picked.  Clearing it forgets the saved tool at once, so a user who unticks
// it and walks away doesn't get auto-started anyway.
static void on_remember_toggle(lv_event_t *e) {
    lv_obj_t *cb = (lv_obj_t *)lv_event_get_target(e);
    s_rememberChoice = lv_obj_has_state(cb, LV_STATE_CHECKED);
    if (!s_rememberChoice) save_tool(TOOL_NONE);
}

static void on_info_toggle(lv_event_t *e) {
    if (!s_infoPopup) return;
    if (lv_obj_has_flag(s_infoPopup, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_infoPopup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_infoPopup, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_tool_button(lv_obj_t *parent, int y, lv_color_t color,
                                  const char *title, const char *subtitle, Tool tool) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 432, 156);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(color, LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 10, 0);
    lv_obj_add_event_cb(btn, on_tool_button, LV_EVENT_CLICKED, (void *)(uintptr_t)tool);

    lv_obj_t *t = lv_label_create(btn);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, title);

    lv_obj_t *s = lv_label_create(btn);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s, lv_color_hex(0xdde4ea), 0);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s, subtitle);
    return btn;
}

static void info_line(lv_obj_t *parent, const char *caption, const char *value,
                      const lv_font_t *font) {
    lv_obj_t *c = lv_label_create(parent);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(c, MENU_CAPTION, 0);
    lv_obj_set_style_pad_top(c, 6, 0);
    lv_label_set_text(c, caption);

    lv_obj_t *v = lv_label_create(parent);
    lv_obj_set_style_text_font(v, font, 0);
    lv_obj_set_style_text_color(v, MENU_VALUE, 0);
    lv_label_set_text(v, value);
}

// Full-screen dimmed backdrop with a card in the middle.  The card and its
// contents aren't clickable, so a tap anywhere -- card included -- lands on
// the backdrop and closes it.
static void build_info_popup(lv_obj_t *scr) {
    s_infoPopup = lv_obj_create(scr);
    lv_obj_remove_style_all(s_infoPopup);
    lv_obj_set_size(s_infoPopup, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_infoPopup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_infoPopup, LV_OPA_70, 0);
    lv_obj_add_flag(s_infoPopup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_infoPopup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_infoPopup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_infoPopup, on_info_toggle, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(s_infoPopup);
    lv_obj_set_size(card, 400, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, MENU_CARD, 0);
    lv_obj_set_style_border_color(card, MENU_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 22, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(card);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_pad_bottom(name, 4, 0);
    lv_label_set_text(name, "LinkScout");

    info_line(card, "BUILT", __DATE__ "  " __TIME__, &lv_font_montserrat_20);
    info_line(card, "WRITTEN BY", "Michael Caldwell-Waller", &lv_font_montserrat_24);

    lv_obj_t *cap = lv_label_create(card);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cap, MENU_CAPTION, 0);
    lv_obj_set_style_pad_top(cap, 6, 0);
    lv_label_set_text(cap, "ON GITHUB");

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_top(row, 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mark = lv_image_create(row);
    lv_image_set_src(mark, &github_mark);

    lv_obj_t *gh = lv_label_create(row);
    lv_obj_set_style_text_font(gh, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(gh, MENU_VALUE, 0);
    lv_obj_set_style_text_decor(gh, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_label_set_text(gh, "github.com/chipguyhere");

    lv_obj_t *notices = lv_label_create(card);
    lv_obj_set_width(notices, lv_pct(100));
    lv_label_set_long_mode(notices, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(notices, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(notices, MENU_CAPTION, 0);
    lv_obj_set_style_pad_top(notices, 16, 0);
    lv_label_set_text(notices,
        "Built with LVGL (MIT), the Montserrat and Font Awesome fonts "
        "(SIL OFL 1.1), ESP-IDF (Apache-2.0) and the Arduino-ESP32 core "
        "(LGPL-2.1). Source code and full notices: see the GitHub page.\n\n"
        "Crestron and Cresnet are trademarks of Crestron Electronics, Inc.; "
        "Cisco and CDP of Cisco Systems, Inc. This tool is not affiliated "
        "with or endorsed by either.\n\n"
        "Tap anywhere to close.");
}

static void build_menu(lv_obj_t *scr, bool remembered) {
    lv_obj_set_style_bg_color(scr, MENU_SCREEN, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *heading = lv_label_create(scr);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(heading, MENU_CAPTION, 0);
    lv_obj_set_style_text_letter_space(heading, 2, 0);
    lv_label_set_text(heading, "CHOOSE A TOOL");
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 30);

    make_tool_button(scr, 62, BTN_CRESNET, "Cresnet Monitor",
                     "Passive RS485 bus watch:\nevery ID, polled and answering",
                     TOOL_CRESNET);
    make_tool_button(scr, 232, BTN_LLDP, "Switch Port Finder",
                     "LLDP / CDP neighbour, VLAN\nand DHCP on the Ethernet port",
                     TOOL_LLDP);

    // Pre-ticked when a tool is already remembered: the menu is only showing
    // because the reset button was pressed, and the box says how to stop that.
    lv_obj_t *cb = lv_checkbox_create(scr);
    lv_checkbox_set_text(cb, "Always start this tool at power-on\n"
                             "(reset button returns to this menu)");
    lv_obj_set_style_text_font(cb, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cb, MENU_CAPTION, 0);
    lv_obj_align(cb, LV_ALIGN_BOTTOM_LEFT, 24, -14);
    lv_obj_set_ext_click_area(cb, 12);
    if (remembered) lv_obj_add_state(cb, LV_STATE_CHECKED);
    s_rememberChoice = remembered;
    lv_obj_add_event_cb(cb, on_remember_toggle, LV_EVENT_VALUE_CHANGED, nullptr);

    // Dim on purpose: it's there for whoever goes looking, not to compete
    // with the two buttons.  The enlarged hit area makes up for the small
    // text on a finger-driven panel.
    lv_obj_t *info = lv_label_create(scr);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(info, MENU_INFO, 0);
    lv_label_set_text(info, "Info");
    lv_obj_align(info, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
    lv_obj_add_flag(info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(info, 20);
    lv_obj_add_event_cb(info, on_info_toggle, LV_EVENT_CLICKED, nullptr);

    build_info_popup(scr);   // last, so it stacks above everything
}

// Swaps the menu out for the chosen tool's screen.  The new screen is built
// off-screen, then loaded with auto-delete so the menu and its popup go away.
//
// The screen is painted before any slow hardware bring-up: ETH.begin() blocks
// for about 4 s, and without the forced refresh that's 4 s of a frozen menu --
// or, when the tool auto-starts, of a black screen.
static void start_tool(Tool tool) {
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_infoPopup = nullptr;

    switch (tool) {
        case TOOL_CRESNET: cresnet_tool_begin(scr); break;
        case TOOL_LLDP:    lldp_tool_begin(scr);    break;
        default: return;
    }
    s_activeTool = tool;
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    lv_refr_now(nullptr);

    if (tool == TOOL_LLDP) lldp_tool_start_network();
}

void setup() {
    Serial0.begin(115200);

    lv_setup.begin();   // also opens the I2C bus the sentinel lives on
    Serial0.printf("LVGL initialized at %dx%d\n", display.width(), display.height());

    // Only a "power-on" with the sentinel already set is the reset button.
    // Every other reason -- a real power-up, but also a panic, watchdog or
    // brownout, or the display driver's restart-on-failure (which comes back
    // as a watchdog reset) -- goes back into the saved tool, because nobody
    // asked for the menu.  If the codec can't be read, show the menu: better a
    // menu at every power-up than a unit that can never get back to it.
    esp_reset_reason_t reason = esp_reset_reason();
    SentinelState sentinel = boot_sentinel_check_and_arm();
    bool resetButton = (reason == ESP_RST_POWERON && sentinel == SENTINEL_PRESENT) ||
                       sentinel == SENTINEL_UNAVAILABLE;
    Tool saved = load_saved_tool();
    Serial0.printf("Reset reason %d, sentinel %s -> %s; saved tool %d\n",
                   (int)reason,
                   sentinel == SENTINEL_PRESENT ? "present" :
                   sentinel == SENTINEL_ABSENT  ? "absent"  : "unavailable",
                   resetButton ? "menu" : "power-on/other", (int)saved);

    if (saved != TOOL_NONE && !resetButton) {
        start_tool(saved);
    } else {
        build_menu(lv_screen_active(), saved != TOOL_NONE);
    }
}

void loop() {
    lv_timer_handler();

    if (s_activeTool == TOOL_NONE && s_pendingTool != TOOL_NONE) {
        start_tool(s_pendingTool);
    }

    if (s_activeTool == TOOL_LLDP) lldp_tool_poll();

    delay(5);
}
