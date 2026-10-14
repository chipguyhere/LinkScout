# chipguy_P486Panel_480display — engineering notes

What this library does differently from the native-resolution
[`chipguy_P486Panel_display`](../chipguy_P486Panel_display). Everything about the board —
power, relays, RS485, Ethernet, audio, the backlight arithmetic — and the shared parts of
the display bring-up (DSI link, timing, panel init, the warm-boot black-panel history) are
in that library's [README-engineering.md](../chipguy_P486Panel_display/README-engineering.md)
and [schematic-description.md](../chipguy_P486Panel_display/schematic-description.md).
Same board, same panel, one copy.

## The PPA scaling pipeline

LVGL renders into two **480×480** RGB565 draw buffers in PSRAM (460,800 bytes each,
128-byte aligned for the P4's PSRAM cache line), in DIRECT mode. The panel still scans out
two native 720×720 DMA framebuffers. Each flush:

1. `esp_cache_msync` C2M on the 480×480 source;
2. `ppa_do_scale_rotate_mirror`, blocking, **scale 1.5 × 1.5**, the rotation angle folded
   into the same operation, into the back framebuffer;
3. M2C on the 720×720 target;
4. page flip (`setActiveFramebuffer(…, false)`).

Because the PPA runs every frame anyway, **rotation costs nothing extra** and can change at
any time — unlike the native library, where rotation is a second pipeline that must be
chosen before `begin()`. The cost is one full-frame scale pass per flush: the PPA reads
460,800 bytes and writes 1,036,800, all over the PSRAM bus the DPI engine is scanning out
of. LVGL has 44 % of the native pixels to render, which is the trade.

A 1.5× scale is non-integer, so the PPA interpolates. One-pixel lines and small text are
softer than at native resolution. Lay out with that in mind, or use the native library for
pixel-exact work.

## Warm-boot handling

The same as the native library, which documents it in full:
[the warm-boot black panel](../chipguy_P486Panel_display/README-engineering.md#the-warm-boot-black-panel-and-the-two-mechanisms-against-it).
The two drivers' `_recoveryResetPulse()`, `_shouldPromoteWarmBootReset()` and
`_triggerDeepestReset()` are the same code. The trials behind the fix were run with this
library: 2026-08-18, the super-watchdog promotion from a running link recovered **20/20**
black panels across 20 soft resets, against **0/11** with an orderly DSI teardown first.

The investigation also built a panel liveness probe (async DCS readback, including a
scanline counter sampled twice), a four-step manual recovery ladder, non-black pixel
counters and a teardown switch. Once the fix was settled they were **removed** from the
library and from the sketch that drove them. Two findings from that work are worth
keeping:

- **A DCS read over this link can black a working panel.** The read forces the link out
  of high-speed video into low-power mode for a bus turnaround, and the picture was seen
  dying within a second of a read. Don't add panel readback to a shipping sketch.
- **The IDF's DSI generic read has no timeout.** `mipi_dsi_hal_host_gen_read_short_packet`
  busy-waits on the host status register and returns void, so a read from a panel that
  never answers hangs the calling task forever. Any future diagnostic that reads from the
  panel has to run on a throwaway task with a deadline.

## What was not derived

Everything board-level: see the native library's
[What was not derived](../chipguy_P486Panel_display/README-engineering.md#what-was-not-derived).
