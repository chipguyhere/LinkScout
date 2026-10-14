/*
 * neighbor.h - Link-layer discovery decoding: LLDP (IEEE 802.1AB) and Cisco CDP.
 *
 * Pure parsing, no hardware dependencies.  Feed whole Ethernet frames to
 * neighbor_parse_frame(); it recognizes the two discovery protocols and fills
 * in a NeighborInfo describing the upstream switch and the port we're plugged
 * into.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

enum NeighborProto : uint8_t {
    NEIGHBOR_NONE = 0,
    NEIGHBOR_LLDP = 1,
    NEIGHBOR_CDP  = 2,
};

// Everything we're willing to show about the neighbor.  Fixed-size strings so
// the whole thing can live in static storage and be memcpy'd between the
// Ethernet RX task and the LVGL loop.
struct NeighborInfo {
    NeighborProto proto = NEIGHBOR_NONE;
    uint32_t rxMillis   = 0;   // millis() when the frame was decoded
    uint32_t epoch      = 0;   // link generation the frame arrived on (see ethsniff)
    uint16_t ttl        = 0;   // neighbor's advertised hold time, seconds
    uint16_t vlan       = 0;   // native/port VLAN, 0 = not advertised
    bool portIsHwAddr   = false;  // port ID was a MAC/network address, not a name
    bool deviceFromChassis = false;  // no system name TLV; `device` is the chassis ID

    char device[64]    = {0};  // LLDP system name  / CDP device ID
    char port[64]      = {0};  // LLDP port ID      / CDP port ID
    char portDescr[64] = {0};  // LLDP port description (often the real ifName)
    char platform[96]  = {0};  // LLDP system description / CDP platform
    char software[80]  = {0};  // CDP software version, first line only
    char mgmt[46]      = {0};  // management address
    char srcMac[18]    = {0};  // source MAC of the frame itself
    char duplex[8]     = {0};  // CDP duplex

    void clear();
};

// Decodes one Ethernet frame.  Returns true only when the frame was LLDP or
// CDP and `out` has been populated; otherwise `out` is untouched.
bool neighbor_parse_frame(const uint8_t *frame, size_t len, NeighborInfo *out);

// The most human-meaningful port name available.  Switches vary: some put
// "GigabitEthernet1/0/12" in the LLDP port ID, others put a MAC there and the
// interface name in the port description.
inline const char *neighbor_port_label(const NeighborInfo &n) {
    if (n.portIsHwAddr && n.portDescr[0]) return n.portDescr;
    if (n.port[0]) return n.port;
    return n.portDescr;
}

inline const char *neighbor_proto_name(NeighborProto p) {
    switch (p) {
        case NEIGHBOR_LLDP: return "LLDP";
        case NEIGHBOR_CDP:  return "CDP";
        default:            return "";
    }
}
