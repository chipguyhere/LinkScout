// cresnet_grid.hpp — 16x16 LVGL grid showing each possible Cresnet ID,
// with a centred heading band along the top of the 480x480 panel.
//
// Layout:
//   y = 0..HEADER_PX-1   centred title (set via setTitle())
//   y = HEADER_PX..479   16 rows of CELL_H-tall cells, CELL_W wide each
//
// Cells use lowercase hex (`%02x`) because the Montserrat font's
// uppercase A-F are wide enough that two-character cells crowd badly.
//
// Reserved Cresnet IDs (0x00, 0x01, 0x02, 0xFF) have no cell at all —
// they can never legitimately appear as a peripheral, so they don't
// occupy a slot in the visible grid.
//
// Text colour reflects the live status from a CresnetMonitor:
//   ALIVE      -> green
//   MISSED     -> yellow   (was alive, but the most recent poll missed)
//   SILENT     -> red      (was alive earlier in this run, now silent)
//   NEVER_SEEN -> magenta  (polled but has never responded since boot)
//   NOT_POLLED -> dim grey #303030 (visible but unobtrusive on black bg)
//
// An LVGL timer wakes the grid periodically and only repaints cells
// whose status actually changed.

#pragma once

#include "lvgl.h"
#include "cresnet_monitor.h"

class CresnetGrid {
public:
  // Build the heading + 16x16 grid as children of `parent` and start an
  // LVGL timer that refreshes the cells from `monitor`.
  void begin(lv_obj_t* parent, CresnetMonitor* monitor,
             uint32_t refresh_ms = 200) {
    _monitor = monitor;

    // Make the parent solid black, no padding, no scroll.
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Centred title label sitting in the top HEADER_PX of the screen.
    // Default to a generic "Cresnet Monitor" so we show something
    // sensible even when the MAC handler never fires (NetClient
    // disabled, MAC doesn't match any known unit, etc.). When a MAC
    // *does* match a known unit, the .ino's MAC handler will call
    // setTitle() to overwrite this with the unit-specific name.
    _title = lv_label_create(parent);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(_title, lv_color_white(), 0);
    lv_label_set_text(_title, "Cresnet Monitor");
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, TITLE_TOP_PAD);

    // 16x16 grid below the heading.
    for (int i = 0; i < 256; ++i) {
      // Reserved IDs never appear on the bus as real peripherals
      // (0x00, 0x01, 0x02 are master-side; 0xFF is broadcast). We do
      // not even render the dim placeholder for them.
      if (i == 0x00 || i == 0x01 || i == 0x02 || i == 0xFF) {
        _labels[i] = nullptr;
        _last_status[i] = (CresnetMonitor::Status)0xFF;
        continue;
      }

      int row = i / GRID_DIM;
      int col = i % GRID_DIM;

      // Transparent cell container so the label can use lv_obj_center()
      // to land cleanly in the middle of its cell.
      lv_obj_t* cell = lv_obj_create(parent);
      lv_obj_remove_style_all(cell);
      lv_obj_set_size(cell, CELL_W, CELL_H);
      lv_obj_set_pos(cell, col * CELL_W, HEADER_PX + row * CELL_H);
      lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
      // Make the cell transparent to clicks so screen-level taps still
      // reach the parent (and toggle the legend overlay).
      lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t* lbl = lv_label_create(cell);
      // Lowercase hex digits: Montserrat's A-F glyphs are wide enough
      // that 2-character labels crowd one another in a 30 px cell;
      // a-f are noticeably narrower.
      lv_label_set_text_fmt(lbl, "%02x", i);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_color(lbl, COLOR_DIM, 0);
      lv_obj_center(lbl);

      _labels[i] = lbl;
      // Sentinel that won't equal any real Status, so the first
      // refresh() always writes the correct colour for every cell.
      _last_status[i] = (CresnetMonitor::Status)0xFF;
    }

    // "Master Offline" overlay — centered red-on-black plate that
    // appears whenever the monitor stops seeing bus activity. Created
    // last so it sits on top of the grid in z-order; hidden by default.
    // Two stacked lines: the headline, and a smaller wiring hint for
    // someone connecting the bus for the first time.
    _offline_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(_offline_overlay);
    lv_obj_set_size(_offline_overlay, OFFLINE_W, OFFLINE_H);
    lv_obj_center(_offline_overlay);
    lv_obj_set_style_bg_color(_offline_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_offline_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_offline_overlay,
                                  LV_COLOR_MAKE(0xFF, 0x00, 0x00), 0);
    lv_obj_set_style_border_width(_offline_overlay, 3, 0);
    lv_obj_set_style_pad_all(_offline_overlay, 0, 0);
    lv_obj_set_style_pad_row(_offline_overlay, 4, 0);
    lv_obj_set_flex_flow(_offline_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_offline_overlay,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(_offline_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_offline_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* offline_lbl = lv_label_create(_offline_overlay);
    lv_label_set_text(offline_lbl, "Master Offline");
    lv_obj_set_style_text_font(offline_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(offline_lbl,
                                LV_COLOR_MAKE(0xFF, 0x00, 0x00), 0);

    lv_obj_t* wiring_lbl = lv_label_create(_offline_overlay);
    lv_label_set_text(wiring_lbl, "Cresnet to RS485: A=Y B=Z");
    lv_obj_set_style_text_font(wiring_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wiring_lbl,
                                LV_COLOR_MAKE(0xFF, 0x00, 0x00), 0);

    // Initialize the bus-activity tracker so we don't trip "offline"
    // on the very first refresh tick.
    _last_total_bytes  = _monitor->totalBytes();
    _last_activity_ms  = millis();

    // Tap-toggleable legend overlay. Created last so it stacks above
    // the grid (and above the offline overlay) when shown. Hidden by
    // default; the parent's click handler flips its visibility.
    _legend = lv_obj_create(parent);
    lv_obj_remove_style_all(_legend);
    lv_obj_set_size(_legend, LEGEND_W, LV_SIZE_CONTENT);
    lv_obj_center(_legend);
    lv_obj_set_style_bg_color(_legend, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_legend, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_legend, lv_color_white(), 0);
    lv_obj_set_style_border_width(_legend, 2, 0);
    lv_obj_set_style_pad_all(_legend, 16, 0);
    lv_obj_set_style_pad_row(_legend, 6, 0);
    lv_obj_set_flex_flow(_legend, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_legend,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(_legend, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_legend, LV_OBJ_FLAG_CLICKABLE);  // tap falls through
    lv_obj_add_flag(_legend, LV_OBJ_FLAG_HIDDEN);

    // Title line.
    {
      lv_obj_t* hdr = lv_label_create(_legend);
      lv_label_set_text(hdr, "Legend");
      lv_obj_set_style_text_font(hdr, &lv_font_montserrat_22, 0);
      lv_obj_set_style_text_color(hdr, lv_color_white(), 0);
      lv_obj_set_style_pad_bottom(hdr, 6, 0);
    }
    // One coloured line per status, matching the colours used in the grid.
    addLegendEntry("Green: alive (responding)",        COLOR_ALIVE);
    addLegendEntry("Yellow: response just missed",     COLOR_MISSED);
    addLegendEntry("Red: was responding, now silent",  COLOR_SILENT);
    addLegendEntry("Magenta: polled, never seen",      COLOR_NEVER_SEEN);
    addLegendEntry("Grey: not currently polled",       COLOR_DIM);

    // Any tap anywhere on the screen toggles the legend. Cells were
    // made non-clickable above, so taps over the grid bubble up here.
    lv_obj_add_event_cb(parent, _click_cb, LV_EVENT_CLICKED, this);

    lv_timer_create(_timer_cb, refresh_ms, this);
  }

  // Set (or change) the heading text. `font` is optional; passing
  // nullptr keeps the current font.
  void setTitle(const char* text, const lv_font_t* font = nullptr) {
    if (!_title) return;
    if (font) {
      lv_obj_set_style_text_font(_title, font, 0);
    }
    lv_label_set_text(_title, text);
    // Re-anchor in case the new text/font changed the label's size.
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, TITLE_TOP_PAD);
  }

  // Force an immediate repaint of every cell. Normally not needed —
  // the timer keeps colours in sync.
  void refresh() {
    if (!_monitor) return;
    CresnetMonitor::Status snap[256];
    _monitor->snapshot(snap);
    for (int i = 0; i < 256; ++i) {
      if (_labels[i] == nullptr) continue;   // reserved-ID slots
      if (snap[i] != _last_status[i]) {
        _last_status[i] = snap[i];
        lv_obj_set_style_text_color(_labels[i], colorFor(snap[i]), 0);
      }
    }

    // Bus-activity heartbeat: any new byte from the wire bumps the
    // last-activity timestamp; OFFLINE_GRACE_MS without any bytes
    // means the master is silent (cable unplugged, processor in
    // reset, etc.) and the overlay pops up.
    uint32_t total = _monitor->totalBytes();
    uint32_t now   = millis();
    if (total != _last_total_bytes) {
      _last_total_bytes = total;
      _last_activity_ms = now;
    }
    bool offline = (uint32_t)(now - _last_activity_ms) > OFFLINE_GRACE_MS;
    if (_offline_overlay) {
      bool currently_hidden =
          lv_obj_has_flag(_offline_overlay, LV_OBJ_FLAG_HIDDEN);
      if (offline && currently_hidden) {
        lv_obj_remove_flag(_offline_overlay, LV_OBJ_FLAG_HIDDEN);
      } else if (!offline && !currently_hidden) {
        lv_obj_add_flag(_offline_overlay, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

private:
  static constexpr int GRID_DIM      = 16;
  static constexpr int HEADER_PX     = 48;          // reserved at top
  static constexpr int TITLE_TOP_PAD = 8;           // vertical offset of title
  static constexpr int CELL_W        = 30;          // 480 / 16
  static constexpr int CELL_H        = 27;          // (480 - HEADER_PX) / 16

  // "Master Offline" overlay sizing and the no-activity threshold.
  static constexpr int      OFFLINE_W         = 340;
  static constexpr int      OFFLINE_H         = 96;
  static constexpr uint32_t OFFLINE_GRACE_MS  = 5000;

  // Tap-toggleable legend overlay sizing.
  static constexpr int      LEGEND_W          = 360;

  // Defined as static const inline so we can keep this class header-only.
  static inline const lv_color_t COLOR_ALIVE      = LV_COLOR_MAKE(0x00, 0xFF, 0x00);
  static inline const lv_color_t COLOR_MISSED     = LV_COLOR_MAKE(0xFF, 0xFF, 0x00);
  static inline const lv_color_t COLOR_SILENT     = LV_COLOR_MAKE(0xFF, 0x00, 0x00);
  static inline const lv_color_t COLOR_NEVER_SEEN = LV_COLOR_MAKE(0xFF, 0x00, 0xFF);
  static inline const lv_color_t COLOR_DIM        = LV_COLOR_MAKE(0x30, 0x30, 0x30);

  static lv_color_t colorFor(CresnetMonitor::Status s) {
    switch (s) {
      case CresnetMonitor::ALIVE:      return COLOR_ALIVE;
      case CresnetMonitor::MISSED:     return COLOR_MISSED;
      case CresnetMonitor::SILENT:     return COLOR_SILENT;
      case CresnetMonitor::NEVER_SEEN: return COLOR_NEVER_SEEN;
      default:                         return COLOR_DIM;
    }
  }

  static void _timer_cb(lv_timer_t* t) {
    static_cast<CresnetGrid*>(lv_timer_get_user_data(t))->refresh();
  }

  // One coloured line inside the legend panel.
  void addLegendEntry(const char* text, lv_color_t color) {
    lv_obj_t* lbl = lv_label_create(_legend);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
  }

  // Tap handler — toggles the legend overlay's visibility.
  static void _click_cb(lv_event_t* e) {
    auto* self = static_cast<CresnetGrid*>(lv_event_get_user_data(e));
    if (!self || !self->_legend) return;
    if (lv_obj_has_flag(self->_legend, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_remove_flag(self->_legend, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(self->_legend, LV_OBJ_FLAG_HIDDEN);
    }
  }

  CresnetMonitor* _monitor          = nullptr;
  lv_obj_t*       _title            = nullptr;
  lv_obj_t*       _offline_overlay  = nullptr;
  lv_obj_t*       _legend           = nullptr;
  lv_obj_t*       _labels[256]      = {};
  CresnetMonitor::Status _last_status[256] = {};
  uint32_t        _last_total_bytes = 0;
  uint32_t        _last_activity_ms = 0;
};
