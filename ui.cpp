/*
 * ui.cpp - LVGL screen for the LLDP/CDP neighbor display.
 *
 * Three stacked bands in the logical 480x480 space:
 *
 *   +--------------------------------------+
 *   |  LINK UP - 100 Mbps - Full           |  live link state
 *   +--------------------------------------+
 *   |  CONNECTED TO - SYSTEM NAME (LLDP)   |
 *   |  sw-idf-2.example.net                |
 *   |  PORT - PORT ID (LLDP)               |
 *   |  GigabitEthernet1/0/12               |
 *   |  System description (LLDP)           |  captioned field blocks
 *   |  Cisco IOS Software, C3750E Softw... |
 *   |  ...are (C3750E-UNIVERSALK9-M)       |
 *   |  Native VLAN (CDP)                   |
 *   |  20                                  |
 *   +--------------------------------------+
 *   |  IP 192.168.1.57                     |  our DHCP lease
 *   +--------------------------------------+
 *
 * Every value is captioned with the protocol field it came from, because the
 * raw strings switches emit are often unfamiliar -- a technician reading
 * "USM8P60 - 8GE, 2.1.8.971" should be able to see that it's the LLDP System
 * Description and not, say, a port name.  Captions are generated per render
 * from UiState::sources, since a switch running both protocols contributes
 * different fields from each.
 *
 * Layout: everything is an LVGL flex column.  Each field is a block -- caption
 * on its own line, value beneath it at the card's full width -- and every
 * height is content-driven, so a long value wraps to as many lines as it needs
 * and pushes the blocks below it down.  Nothing is truncated.  Blocks with no
 * data hide, which removes them from the flex layout entirely.  If a very
 * chatty switch ever overflows the card, it scrolls vertically by touch rather
 * than clipping.
 *
 * Staleness: when the link drops, everything the network told us is still on
 * screen but drawn in grey, because it describes a link that no longer exists.
 * It only returns to full colour when that particular fact is re-learned on
 * the current link -- a fresh LLDP/CDP advertisement for the neighbour panel,
 * a fresh DHCP lease for the address panel.  The link band itself is always
 * live and never greyed -- except for the boot window, where "no link yet" is
 * a grey STARTING ETHERNET rather than a red LINK DOWN, because at that point
 * nothing has gone wrong yet.
 */

#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#include "ui.h"

// ------------------------------------------------------------- palette ----

#define COL_SCREEN     lv_color_hex(0x0d1117)
#define COL_CARD       lv_color_hex(0x161b22)
#define COL_FOOTER     lv_color_hex(0x11161d)

#define COL_CAPTION    lv_color_hex(0x6b7785)
#define COL_VALUE      lv_color_hex(0xe6edf3)
#define COL_ACCENT     lv_color_hex(0x58d0f5)
#define COL_WARN       lv_color_hex(0xd9a441)

// Greyed-out equivalents.  The hierarchy is preserved -- captions stay dimmer
// than values -- so the layout still reads, just visibly inert.
#define COL_CAPTION_S  lv_color_hex(0x454c55)
#define COL_VALUE_S    lv_color_hex(0x707880)
#define COL_ACCENT_S   lv_color_hex(0x878f98)

#define COL_LINK_UP    lv_color_hex(0x1a7f4b)
#define COL_LINK_DOWN  lv_color_hex(0x8f2226)
// Neutral grey for the boot window: no link yet, but nothing is wrong either.
#define COL_LINK_START lv_color_hex(0x3d444d)

// ------------------------------------------------------------- widgets ----

enum Role : uint8_t { ROLE_CAPTION, ROLE_VALUE, ROLE_ACCENT };

// Every label carries a copy of the text last written to it, so a render pass
// that changes nothing touches no widgets.  Repainting costs a full 480x480
// render plus a PPA scale, so it's worth avoiding four times a second.
struct Lbl {
    lv_obj_t *obj  = nullptr;
    Role      role = ROLE_CAPTION;
    char      last[128] = {0};
};

// Field blocks, in fixed screen order.  Each is a caption/value pair; the two
// headline blocks just use bigger type.  Blocks with nothing to show are
// hidden and drop out of the flex flow.
enum {
    BLK_DEVICE,     // headline: who we're plugged into
    BLK_PORT,       // headline: which port
    BLK_PORTALT,    // whichever of port ID / port description isn't the headline
    BLK_DESCR,      // LLDP system description, or CDP platform
    BLK_SOFTWARE,   // CDP software version
    BLK_VLAN,
    BLK_MGMT,
    BLK_DUPLEX,
    BLK_COUNT
};

// Label table.  Indices 0..L_NEIGHBOR_END-1 grey out together on link loss;
// L_NEIGHBOR_END..L_LOCAL_END belong to the address panel and grey on their
// own schedule.  L_LINK sits outside both -- link state is always live.
enum {
    L_BLK_BASE = 0,
    L_AGE = L_BLK_BASE + BLK_COUNT * 2,
    L_NEIGHBOR_END,
    L_IP = L_NEIGHBOR_END, L_MAC,
    L_LOCAL_END,
    L_LINK = L_LOCAL_END,
    L_COUNT
};
#define L_CAP(b) (L_BLK_BASE + (b) * 2)
#define L_VAL(b) (L_BLK_BASE + (b) * 2 + 1)

static Lbl s_lbl[L_COUNT];
static lv_obj_t *s_blk[BLK_COUNT];
static lv_obj_t *bar, *card, *footer;

// Cached so we only touch styles when something actually changes.
static int s_lastNeighborStale = -1;
static int s_lastLocalStale    = -1;
static int s_lastAgeWarn       = -1;
static int s_lastLinkPhase     = -1;   // LINK_DOWN / LINK_UP / LINK_STARTING

// --------------------------------------------------------------- helpers --

// All labels wrap and self-size; none get an explicit height, which is what
// lets a long value grow downward instead of being clipped.
static void init_label(int idx, lv_obj_t *parent, const lv_font_t *font, Role role) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, "");
    s_lbl[idx].obj = l;
    s_lbl[idx].role = role;
    s_lbl[idx].last[0] = '\0';
}

static void set_text(int idx, const char *txt) {
    Lbl &l = s_lbl[idx];
    if (strncmp(l.last, txt, sizeof(l.last)) == 0) return;
    strncpy(l.last, txt, sizeof(l.last) - 1);
    l.last[sizeof(l.last) - 1] = '\0';
    lv_label_set_text(l.obj, txt);
}

static void apply_palette(int from, int to, bool stale) {
    for (int i = from; i < to; i++) {
        if (!s_lbl[i].obj) continue;
        lv_color_t c;
        switch (s_lbl[i].role) {
            case ROLE_ACCENT:  c = stale ? COL_ACCENT_S  : COL_ACCENT;  break;
            case ROLE_VALUE:   c = stale ? COL_VALUE_S   : COL_VALUE;   break;
            default:           c = stale ? COL_CAPTION_S : COL_CAPTION; break;
        }
        lv_obj_set_style_text_color(s_lbl[i].obj, c, 0);
    }
}

static void set_hidden(lv_obj_t *obj, bool hide) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hide) return;
    if (hide) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// "12 s ago" / "4 m ago" / "1 h 20 m ago"
static void format_age(char *dst, size_t dstsz, uint32_t ageSec) {
    if (ageSec < 60)        snprintf(dst, dstsz, "%u s ago", (unsigned)ageSec);
    else if (ageSec < 3600) snprintf(dst, dstsz, "%u m ago", (unsigned)(ageSec / 60));
    else                    snprintf(dst, dstsz, "%u h %u m ago",
                                     (unsigned)(ageSec / 3600), (unsigned)((ageSec % 3600) / 60));
}

static const char *proto_tag(NeighborProto p) {
    return p == NEIGHBOR_CDP ? "CDP" : "LLDP";
}

// ---------------------------------------------------------------- build ---

// One field block: caption above, value below, both at full card width.  The
// container is height-for-content so the block is exactly as tall as its
// wrapped value needs.
static void mk_block(int b, const lv_font_t *valFont, Role valRole, int padTop) {
    lv_obj_t *c = lv_obj_create(card);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_pad_top(c, padTop, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 1, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_blk[b] = c;

    init_label(L_CAP(b), c, &lv_font_montserrat_12, ROLE_CAPTION);
    init_label(L_VAL(b), c, valFont, valRole);
}

void ui_init(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, COL_SCREEN, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- link band -------------------------------------------------------
    bar = lv_obj_create(scr);
    lv_obj_set_size(bar, lv_pct(100), 56);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_LINK_START, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    init_label(L_LINK, bar, &lv_font_montserrat_22, ROLE_VALUE);
    lv_obj_set_style_text_color(s_lbl[L_LINK].obj, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(s_lbl[L_LINK].obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_lbl[L_LINK].obj);

    // ---- neighbor card ---------------------------------------------------
    card = lv_obj_create(scr);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    // Safety valve: if a switch ever advertises more than fits, it scrolls by
    // touch instead of losing the bottom of the list.
    lv_obj_set_scroll_dir(card, LV_DIR_VER);

    mk_block(BLK_DEVICE,   &lv_font_montserrat_24, ROLE_VALUE,  0);
    mk_block(BLK_PORT,     &lv_font_montserrat_32, ROLE_ACCENT, 6);
    mk_block(BLK_PORTALT,  &lv_font_montserrat_16, ROLE_VALUE,  6);
    mk_block(BLK_DESCR,    &lv_font_montserrat_16, ROLE_VALUE,  0);
    mk_block(BLK_SOFTWARE, &lv_font_montserrat_16, ROLE_VALUE,  0);
    mk_block(BLK_VLAN,     &lv_font_montserrat_16, ROLE_VALUE,  0);
    mk_block(BLK_MGMT,     &lv_font_montserrat_16, ROLE_VALUE,  0);
    mk_block(BLK_DUPLEX,   &lv_font_montserrat_16, ROLE_VALUE,  0);

    init_label(L_AGE, card, &lv_font_montserrat_14, ROLE_CAPTION);
    lv_obj_set_style_pad_top(s_lbl[L_AGE].obj, 4, 0);

    // ---- our own address -------------------------------------------------
    footer = lv_obj_create(scr);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_height(footer, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_bg_color(footer, COL_FOOTER, 0);
    lv_obj_set_style_pad_all(footer, 14, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(footer, 2, 0);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    init_label(L_IP, footer, &lv_font_montserrat_24, ROLE_VALUE);
    init_label(L_MAC, footer, &lv_font_montserrat_14, ROLE_CAPTION);

    apply_palette(0, L_NEIGHBOR_END, false);
    apply_palette(L_NEIGHBOR_END, L_LOCAL_END, false);
}

// --------------------------------------------------------------- render ---

void ui_render(const UiState &s) {
    char buf[192];
    bool used[BLK_COUNT] = {false};

    // Fills one block, skipping it when there's nothing to show.
    auto block = [&](int b, const char *caption, const char *value) {
        if (!value || !value[0]) return;
        set_text(L_CAP(b), caption);
        set_text(L_VAL(b), value);
        used[b] = true;
    };

    // ---- link band -------------------------------------------------------
    // Three states, not two: during the boot window a missing link isn't a
    // fault yet, so it gets a neutral grey band rather than the red one that
    // sends a technician off to check a cable that's perfectly fine.
    if (s.linkStarting) {
        snprintf(buf, sizeof(buf), "STARTING ETHERNET " LV_SYMBOL_REFRESH);
    } else if (!s.linkUp) {
        snprintf(buf, sizeof(buf), "LINK DOWN " LV_SYMBOL_CLOSE " check cable");
    } else {
        snprintf(buf, sizeof(buf), "LINK UP " LV_SYMBOL_OK "  %u Mbps  %s",
                 (unsigned)s.linkSpeed, s.fullDuplex ? "Full" : "Half");
    }
    set_text(L_LINK, buf);

    int linkPhase = s.linkStarting ? 2 : (s.linkUp ? 1 : 0);
    if (linkPhase != s_lastLinkPhase) {
        s_lastLinkPhase = linkPhase;
        lv_obj_set_style_bg_color(bar, linkPhase == 2 ? COL_LINK_START
                                     : linkPhase == 1 ? COL_LINK_UP
                                                      : COL_LINK_DOWN, 0);
    }

    // ---- neighbor --------------------------------------------------------
    // Grey first, so the overdue-advertisement highlight below still wins on
    // the same frame it turns on.
    int wantStale = s.neighborStale ? 1 : 0;
    if (wantStale != s_lastNeighborStale) {
        s_lastNeighborStale = wantStale;
        apply_palette(0, L_NEIGHBOR_END, s.neighborStale);
        s_lastAgeWarn = -1;
    }

    if (!s.haveNeighbor) {
        // Same reasoning as the band: during the boot window, say what we're
        // doing rather than reporting an absence as a finding.
        block(BLK_DEVICE, "CONNECTED TO",
              s.linkStarting ? "Waiting for the link to come up..."
                             : s.linkUp ? "Listening for LLDP / CDP..."
                                        : "No switch information");
        block(BLK_PORT, "PORT", "--");
        set_text(L_AGE, s.linkStarting ? "Bringing up Ethernet"
                                       : s.linkUp ? "Switches advertise every 30-60 s"
                                                  : "Plug into a switch running LLDP or CDP");
    } else {
        const NeighborInfo &n = s.neighbor;
        const FieldSources &src = s.sources;

        // Headline: who, and which TLV that name came out of.  A switch with
        // no System Name TLV falls back to its Chassis ID, which is usually a
        // MAC address -- worth saying so rather than letting it look like a
        // hostname.
        const char *deviceField = (src.device == NEIGHBOR_CDP) ? "DEVICE ID"
                                : n.deviceFromChassis          ? "CHASSIS ID"
                                                               : "SYSTEM NAME";
        snprintf(buf, sizeof(buf), "CONNECTED TO - %s (%s)",
                 deviceField, proto_tag(src.device));
        block(BLK_DEVICE, buf, n.device[0] ? n.device : n.srcMac);

        // Headline port.  neighbor_port_label() prefers the port description
        // when the port ID turned out to be a MAC rather than a name, so the
        // caption has to follow the same choice.
        bool heroIsDescr = n.portIsHwAddr && n.portDescr[0];
        const char *portLabel = neighbor_port_label(n);
        snprintf(buf, sizeof(buf), "PORT - %s (%s)",
                 heroIsDescr ? "PORT DESCRIPTION" : "PORT ID",
                 proto_tag(heroIsDescr ? src.portDescr : src.port));
        block(BLK_PORT, buf, portLabel[0] ? portLabel : "--");

        // ---- detail blocks -----------------------------------------------
        // Whichever port field the headline didn't use.
        if (heroIsDescr) {
            snprintf(buf, sizeof(buf), "Port ID (%s)", proto_tag(src.port));
            block(BLK_PORTALT, buf, n.port);
        } else if (n.portDescr[0] && strcmp(n.portDescr, portLabel) != 0) {
            snprintf(buf, sizeof(buf), "Port description (%s)", proto_tag(src.portDescr));
            block(BLK_PORTALT, buf, n.portDescr);
        }

        snprintf(buf, sizeof(buf), "%s (%s)",
                 src.platform == NEIGHBOR_CDP ? "Platform" : "System description",
                 proto_tag(src.platform));
        block(BLK_DESCR, buf, n.platform);

        snprintf(buf, sizeof(buf), "Software version (%s)", proto_tag(src.software));
        block(BLK_SOFTWARE, buf, n.software);

        if (n.vlan) {
            char vlanStr[12];
            snprintf(vlanStr, sizeof(vlanStr), "%u", (unsigned)n.vlan);
            snprintf(buf, sizeof(buf), "%s (%s)",
                     src.vlan == NEIGHBOR_CDP ? "Native VLAN" : "Port VLAN ID",
                     proto_tag(src.vlan));
            block(BLK_VLAN, buf, vlanStr);
        }

        snprintf(buf, sizeof(buf), "Mgmt address (%s)", proto_tag(src.mgmt));
        block(BLK_MGMT, buf, n.mgmt);

        snprintf(buf, sizeof(buf), "Duplex (%s)", proto_tag(src.duplex));
        block(BLK_DUPLEX, buf, n.duplex);

        // ---- freshness ---------------------------------------------------
        uint32_t ageSec = (millis() - n.rxMillis) / 1000;
        char age[32];
        format_age(age, sizeof(age), ageSec);

        const char *via = (s.lldpSeen && s.cdpSeen) ? "LLDP + CDP"
                                                    : neighbor_proto_name(n.proto);
        bool expired = n.ttl && ageSec > n.ttl;
        snprintf(buf, sizeof(buf), "%s   %s%s", via, age, expired ? "   (expired)" : "");
        set_text(L_AGE, buf);

        // Amber for an overdue advertisement, but only while the panel is
        // otherwise live -- grey wins when the data predates this link.
        int wantWarn = (expired && !s.neighborStale) ? 1 : 0;
        if (wantWarn != s_lastAgeWarn) {
            s_lastAgeWarn = wantWarn;
            lv_obj_set_style_text_color(
                s_lbl[L_AGE].obj,
                wantWarn ? COL_WARN : (s.neighborStale ? COL_CAPTION_S : COL_CAPTION), 0);
        }
    }

    for (int b = 0; b < BLK_COUNT; b++) set_hidden(s_blk[b], !used[b]);

    // ---- our address -----------------------------------------------------
    if (s.haveIp) {
        snprintf(buf, sizeof(buf), "IP  %s", s.ip);
        set_text(L_IP, buf);
    } else {
        set_text(L_IP, s.linkUp ? "IP  requesting via DHCP..." : "IP  --");
    }
    snprintf(buf, sizeof(buf), "MAC  %s      %u frames", s.mac, (unsigned)s.framesSeen);
    set_text(L_MAC, buf);

    int wantLocalStale = s.ipStale ? 1 : 0;
    if (wantLocalStale != s_lastLocalStale) {
        s_lastLocalStale = wantLocalStale;
        apply_palette(L_NEIGHBOR_END, L_LOCAL_END, s.ipStale);
    }
}
