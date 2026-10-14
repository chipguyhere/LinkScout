# Third-party notices

The firmware built from this sketch contains code and data from the projects below, in
addition to the sketch's own code (MIT, see [LICENSE](LICENSE)). Full license texts are in
[`LICENSES/`](LICENSES). Distribute this file and that folder with every copy of the firmware
binary.

Versions are the ones the released binary was built with; see `BUILD_INFO.txt` in the repository
for the exact set.

| Component | Version | License | Copyright | Text |
| --- | --- | --- | --- | --- |
| Arduino core for ESP32 (`arduino-esp32`) | 3.3.11 | LGPL-2.1 (parts Apache-2.0) | Espressif Systems and contributors; Arduino LLC and contributors | [LGPL-2.1](LICENSES/LGPL-2.1.txt), [Apache-2.0](LICENSES/Apache-2.0.txt) |
| ESP-IDF, as pre-built by the core | v5.5.5 | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. | [Apache-2.0](LICENSES/Apache-2.0.txt) |
| FreeRTOS kernel (ESP-IDF SMP port) | 10.5.1 | MIT | Amazon.com, Inc. or its affiliates; modifications Espressif Systems | [MIT](LICENSES/MIT-FreeRTOS.txt) |
| lwIP TCP/IP stack (ESP-IDF port) | — | BSD-3-Clause | Swedish Institute of Computer Science and contributors | [BSD](LICENSES/BSD-3-Clause-lwIP.txt) |
| newlib C library (Espressif build) | — | Various BSD-style / permissive | Red Hat, Inc. and others, per file | [COPYING.NEWLIB](LICENSES/COPYING.NEWLIB.txt) |
| TinyUSB (USB CDC) | — | MIT | Ha Thach (tinyusb.org) | [MIT](LICENSES/MIT-TinyUSB.txt) |
| mbedTLS and other ESP-IDF components, where linked | — | Apache-2.0 | The Mbed TLS Contributors; Espressif Systems | [Apache-2.0](LICENSES/Apache-2.0.txt) |
| LVGL graphics library | 9.5.0 | MIT | LVGL Kft | [MIT](LICENSES/MIT-LVGL.txt) |
| Montserrat typeface (LVGL's built-in bitmap fonts) | — | SIL OFL 1.1 | The Montserrat Project Authors | [OFL-1.1](LICENSES/OFL-1.1-Montserrat.txt) |
| Font Awesome Free 5 (icon glyphs in LVGL's built-in fonts) | 5 | Font: SIL OFL 1.1; icons: CC BY 4.0 | Fonticons, Inc. | [OFL-1.1](LICENSES/OFL-1.1-Montserrat.txt) text; CC BY 4.0 at <https://creativecommons.org/licenses/by/4.0/> |
| `chipguy_P486Panel_480display` | 1.0.0 | MIT; `esp_lcd_panel_dpi_bb.*` Apache-2.0 | chipguyhere; Espressif Systems (Shanghai) Co., Ltd. | [LICENSE](LICENSE), [Apache-2.0](LICENSES/Apache-2.0.txt) |
| `chipguy_P486Panel_touch` | 1.0.0 | see note below | chipguyhere | |

## Notes

**Arduino core for ESP32 (LGPL).** The core is linked statically. To meet LGPL-2.1 section 6,
the complete source of this sketch and of the non-public libraries it uses is published (see
[BUILDING.md](BUILDING.md), "Source code and the LGPL"), so you can rebuild the firmware against a modified
copy of the core. The core itself is unmodified and available at
<https://github.com/espressif/arduino-esp32>, tag `3.3.11`.

**ESP-IDF.** The core links a pre-built ESP-IDF (`esp32p4-libs` 3.3.11, IDF v5.5.5). Source:
<https://github.com/espressif/esp-idf>, tag `v5.5.5`. ESP-IDF incorporates further third-party
components under their own licenses. Only those components referenced by this firmware are
linked into it; the major ones are listed above.

**Montserrat font.** The Montserrat font was converted to bitmaps by the LVGL project and is
included under the SIL Open Font License 1.1. The bitmap fonts are not sold separately and do
not use the Reserved Font Name.

**Font Awesome.** Font Awesome Free by Fonticons, Inc., <https://fontawesome.com>. The icons
are licensed under CC BY 4.0 and the font files under SIL OFL 1.1.

**`chipguy_P486Panel_touch`.** The GT911 touch driver follows the API and data structures of
Espressif's `esp_lcd_touch` component (Apache-2.0,
<https://github.com/espressif/esp-bsp/tree/master/components/lcd_touch>).

**GitHub mark.** The GitHub logo on the Info screen is rasterized from
`mark-github-24.svg` in Primer Octicons 19.15.0 (MIT, GitHub Inc.). The mark itself is a
trademark of GitHub, Inc., and is used here only to link to the author's GitHub profile.

## Trademarks

Crestron and Cresnet are trademarks of Crestron Electronics, Inc. Cisco and CDP are
trademarks of Cisco Systems, Inc. Waveshare is a trademark of Waveshare Electronics. GitHub is
a trademark of GitHub, Inc. These names are used only to describe compatibility. This project
is not affiliated with, sponsored by or endorsed by any of these companies.
