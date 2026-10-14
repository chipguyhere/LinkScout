/*
 * neighbor.cpp - LLDP and CDP frame decoding.
 *
 * Both protocols are link-local L2 multicast: a switch emits them out of every
 * port, so whatever arrives on our wire describes the port we're plugged into.
 *
 *   LLDP  dst 01:80:c2:00:00:0e, ethertype 0x88cc, a run of type/length/value
 *         records (7-bit type, 9-bit length packed into a 16-bit header).
 *
 *   CDP   dst 01:00:0c:cc:cc:cc, 802.3 length field + LLC/SNAP
 *         (aa-aa-03, OUI 00-00-0c, protocol 0x2000), then a 4-byte CDP header
 *         and 16-bit type / 16-bit length records whose length covers its own
 *         4-byte header.
 *
 * Everything here is bounds-checked against the frame length; a truncated or
 * malformed frame stops the walk rather than reading past the buffer.
 */

#include <Arduino.h>
#include <string.h>
#include "neighbor.h"

void NeighborInfo::clear() {
    *this = NeighborInfo();
}

// ---------------------------------------------------------------- helpers --

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// Copies up to dstsz-1 bytes, replacing anything unprintable with a space so a
// hostile or binary field can't scribble control codes into the UI.
static void copy_text(char *dst, size_t dstsz, const uint8_t *src, size_t len) {
    if (dstsz == 0) return;
    size_t n = 0;
    for (size_t i = 0; i < len && n < dstsz - 1; i++) {
        uint8_t c = src[i];
        dst[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
    }
    // Trim trailing blanks so centered labels stay centered.
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

// Same, but stops at the first CR/LF.  Cisco's system description and software
// version fields are multi-line banners whose first line is the useful part.
static void copy_first_line(char *dst, size_t dstsz, const uint8_t *src, size_t len) {
    size_t cut = len;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\r' || src[i] == '\n') { cut = i; break; }
    }
    copy_text(dst, dstsz, src, cut);
}

static void format_mac(char *dst, size_t dstsz, const uint8_t *mac) {
    snprintf(dst, dstsz, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void format_ipv4(char *dst, size_t dstsz, const uint8_t *ip) {
    snprintf(dst, dstsz, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void format_ipv6(char *dst, size_t dstsz, const uint8_t *ip) {
    snprintf(dst, dstsz, "%x:%x:%x:%x:%x:%x:%x:%x",
             rd16(ip), rd16(ip + 2), rd16(ip + 4), rd16(ip + 6),
             rd16(ip + 8), rd16(ip + 10), rd16(ip + 12), rd16(ip + 14));
}

// LLDP encodes addresses as an IANA address-family byte followed by the
// address itself.  Returns false for families we don't render.
static bool format_lldp_addr(char *dst, size_t dstsz, const uint8_t *v, size_t len) {
    if (len < 1) return false;
    if (v[0] == 1 && len >= 5)  { format_ipv4(dst, dstsz, v + 1); return true; }
    if (v[0] == 2 && len >= 17) { format_ipv6(dst, dstsz, v + 1); return true; }
    if (v[0] == 6 && len >= 7)  { format_mac(dst, dstsz, v + 1);  return true; }
    return false;
}

// ------------------------------------------------------------------- LLDP --

// LLDP TLV types we care about.
enum {
    LLDP_TLV_END          = 0,
    LLDP_TLV_CHASSIS_ID   = 1,
    LLDP_TLV_PORT_ID      = 2,
    LLDP_TLV_TTL          = 3,
    LLDP_TLV_PORT_DESCR   = 4,
    LLDP_TLV_SYS_NAME     = 5,
    LLDP_TLV_SYS_DESCR    = 6,
    LLDP_TLV_MGMT_ADDR    = 8,
    LLDP_TLV_ORG_SPECIFIC = 127,
};

static bool parse_lldp(const uint8_t *frame, size_t len, size_t off, NeighborInfo *out) {
    bool haveSysName = false;
    bool any = false;

    size_t p = off;
    while (p + 2 <= len) {
        uint16_t hdr  = rd16(frame + p);
        uint8_t  type = (uint8_t)(hdr >> 9);
        uint16_t tlen = hdr & 0x01ff;
        p += 2;

        if (type == LLDP_TLV_END) break;
        if (p + tlen > len) break;          // truncated frame

        const uint8_t *v = frame + p;
        switch (type) {
            case LLDP_TLV_CHASSIS_ID:
                // Subtype byte, then the ID.  Only used as a fallback name.
                if (tlen >= 2 && !haveSysName) {
                    if (v[0] == 4 && tlen >= 7) {
                        format_mac(out->device, sizeof(out->device), v + 1);
                    } else if (v[0] == 5) {
                        if (!format_lldp_addr(out->device, sizeof(out->device), v + 1, tlen - 1)) {
                            copy_text(out->device, sizeof(out->device), v + 1, tlen - 1);
                        }
                    } else {
                        copy_text(out->device, sizeof(out->device), v + 1, tlen - 1);
                    }
                    out->deviceFromChassis = true;
                }
                any = true;
                break;

            case LLDP_TLV_PORT_ID:
                if (tlen >= 2) {
                    uint8_t subtype = v[0];
                    if (subtype == 3 && tlen >= 7) {           // MAC address
                        format_mac(out->port, sizeof(out->port), v + 1);
                        out->portIsHwAddr = true;
                    } else if (subtype == 4) {                 // network address
                        out->portIsHwAddr = true;
                        if (!format_lldp_addr(out->port, sizeof(out->port), v + 1, tlen - 1)) {
                            copy_text(out->port, sizeof(out->port), v + 1, tlen - 1);
                        }
                    } else {                                   // interface name/alias/local
                        copy_text(out->port, sizeof(out->port), v + 1, tlen - 1);
                    }
                }
                any = true;
                break;

            case LLDP_TLV_TTL:
                if (tlen >= 2) out->ttl = rd16(v);
                break;

            case LLDP_TLV_PORT_DESCR:
                copy_text(out->portDescr, sizeof(out->portDescr), v, tlen);
                break;

            case LLDP_TLV_SYS_NAME:
                if (tlen > 0) {
                    copy_text(out->device, sizeof(out->device), v, tlen);
                    out->deviceFromChassis = false;
                    haveSysName = true;
                }
                break;

            case LLDP_TLV_SYS_DESCR:
                copy_first_line(out->platform, sizeof(out->platform), v, tlen);
                break;

            case LLDP_TLV_MGMT_ADDR:
                // addr-string-length, then that many bytes of
                // (address subtype + address).
                if (tlen >= 2) {
                    uint8_t alen = v[0];
                    if (alen >= 1 && (size_t)alen + 1 <= tlen && out->mgmt[0] == '\0') {
                        format_lldp_addr(out->mgmt, sizeof(out->mgmt), v + 1, alen);
                    }
                }
                break;

            case LLDP_TLV_ORG_SPECIFIC:
                // 3-byte OUI + 1-byte subtype.  802.1 (00-80-c2) subtype 1 is
                // the port VLAN ID.
                if (tlen >= 6 && v[0] == 0x00 && v[1] == 0x80 && v[2] == 0xc2 && v[3] == 0x01) {
                    out->vlan = rd16(v + 4);
                }
                break;

            default:
                break;
        }
        p += tlen;
    }

    return any;
}

// -------------------------------------------------------------------- CDP --

enum {
    CDP_TLV_DEVICE_ID   = 0x0001,
    CDP_TLV_ADDRESSES   = 0x0002,
    CDP_TLV_PORT_ID     = 0x0003,
    CDP_TLV_SOFTWARE    = 0x0005,
    CDP_TLV_PLATFORM    = 0x0006,
    CDP_TLV_NATIVE_VLAN = 0x000a,
    CDP_TLV_DUPLEX      = 0x000b,
};

// CDP address list: a 4-byte count, then per entry a protocol descriptor and a
// 16-bit-length address.  We keep the first IPv4 we see.
static void parse_cdp_addresses(const uint8_t *v, size_t len, NeighborInfo *out) {
    if (len < 4) return;
    uint32_t count = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                     ((uint32_t)v[2] << 8) | v[3];
    size_t p = 4;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 2 > len) return;
        uint8_t protoLen = v[p + 1];
        p += 2;
        if (p + protoLen + 2 > len) return;
        const uint8_t *proto = v + p;
        p += protoLen;
        uint16_t addrLen = rd16(v + p);
        p += 2;
        if (p + addrLen > len) return;

        // NLPID 0xcc is IPv4, 0x86dd (in the 802.2 form) is IPv6.
        if (protoLen == 1 && proto[0] == 0xcc && addrLen == 4) {
            format_ipv4(out->mgmt, sizeof(out->mgmt), v + p);
            return;
        }
        p += addrLen;
    }
}

static bool parse_cdp(const uint8_t *frame, size_t len, size_t off, NeighborInfo *out) {
    // CDP header: version, TTL, checksum.
    if (off + 4 > len) return false;
    out->ttl = frame[off + 1];

    bool any = false;
    size_t p = off + 4;
    while (p + 4 <= len) {
        uint16_t type = rd16(frame + p);
        uint16_t tlen = rd16(frame + p + 2);
        if (tlen < 4 || p + tlen > len) break;   // length covers its own header

        const uint8_t *v = frame + p + 4;
        size_t vlen = tlen - 4;

        switch (type) {
            case CDP_TLV_DEVICE_ID:
                copy_text(out->device, sizeof(out->device), v, vlen);
                any = true;
                break;
            case CDP_TLV_PORT_ID:
                copy_text(out->port, sizeof(out->port), v, vlen);
                any = true;
                break;
            case CDP_TLV_ADDRESSES:
                parse_cdp_addresses(v, vlen, out);
                break;
            case CDP_TLV_SOFTWARE:
                copy_first_line(out->software, sizeof(out->software), v, vlen);
                break;
            case CDP_TLV_PLATFORM:
                copy_text(out->platform, sizeof(out->platform), v, vlen);
                break;
            case CDP_TLV_NATIVE_VLAN:
                if (vlen >= 2) out->vlan = rd16(v);
                break;
            case CDP_TLV_DUPLEX:
                if (vlen >= 1) strcpy(out->duplex, v[0] ? "Full" : "Half");
                break;
            default:
                break;
        }
        p += tlen;
    }

    return any;
}

// ------------------------------------------------------------ entry point --

bool neighbor_parse_frame(const uint8_t *frame, size_t len, NeighborInfo *out) {
    if (len < 60) return false;   // shorter than a legal Ethernet frame

    // Step over any VLAN tags to find the real ethertype / 802.3 length field.
    size_t et = 12;
    for (int i = 0; i < 2; i++) {
        if (et + 2 > len) return false;
        uint16_t t = rd16(frame + et);
        if (t == 0x8100 || t == 0x88a8 || t == 0x9100) et += 4; else break;
    }
    if (et + 2 > len) return false;

    uint16_t ethType = rd16(frame + et);
    size_t payload = et + 2;

    NeighborInfo n;

    if (ethType == 0x88cc) {
        n.proto = NEIGHBOR_LLDP;
        if (!parse_lldp(frame, len, payload, &n)) return false;
    } else if (ethType <= 1500) {
        // 802.3 length field -- check for the LLC/SNAP header CDP rides on.
        // Other Cisco protocols (VTP, DTP, PAgP) share the same destination
        // MAC and differ only in the SNAP protocol ID, so match it exactly.
        if (payload + 8 > len) return false;
        const uint8_t *llc = frame + payload;
        static const uint8_t cdpSnap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x0c, 0x20, 0x00};
        if (memcmp(llc, cdpSnap, sizeof(cdpSnap)) != 0) return false;
        n.proto = NEIGHBOR_CDP;
        if (!parse_cdp(frame, len, payload + 8, &n)) return false;
    } else {
        return false;
    }

    format_mac(n.srcMac, sizeof(n.srcMac), frame + 6);
    n.rxMillis = millis();
    *out = n;
    return true;
}
