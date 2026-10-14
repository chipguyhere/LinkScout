/*
 * ui.h - LVGL screen for the LLDP/CDP neighbor display.
 *
 * The sketch gathers everything worth showing into a UiState and hands it to
 * ui_render() a few times a second; the UI layer only touches widgets whose
 * content actually changed, so an idle screen costs nothing to redraw.
 */

#pragma once

#include "lvgl.h"
#include "neighbor.h"

// Which protocol each displayed field actually came from.  A switch running
// both protocols contributes different fields from each, and the on-screen
// captions name the source so a technician reading an unfamiliar value knows
// exactly which TLV produced it.
struct FieldSources {
    NeighborProto device    = NEIGHBOR_NONE;
    NeighborProto port      = NEIGHBOR_NONE;
    NeighborProto portDescr = NEIGHBOR_NONE;
    NeighborProto platform  = NEIGHBOR_NONE;
    NeighborProto software  = NEIGHBOR_NONE;
    NeighborProto mgmt      = NEIGHBOR_NONE;
    NeighborProto vlan      = NEIGHBOR_NONE;
    NeighborProto duplex    = NEIGHBOR_NONE;
};

struct UiState {
    // Live link state -- never greyed, this is the closest thing the board has
    // to a link LED.  `linkStarting` means no link yet, but we're still inside
    // the post-boot window where that's expected rather than a fault; the band
    // says so in grey instead of crying LINK DOWN in red.
    bool     linkUp       = false;
    bool     linkStarting = false;
    uint16_t linkSpeed    = 0;     // Mbps
    bool     fullDuplex   = false;

    // Our own addressing.  `ipStale` is set when the lease predates the
    // current link.
    bool haveIp   = false;
    bool ipStale  = false;
    char ip[24]   = {0};
    char mac[18]  = {0};

    // What the upstream switch told us.  `neighborStale` is set when the
    // information predates the current link.
    bool         haveNeighbor   = false;
    bool         neighborStale  = false;
    NeighborInfo neighbor;
    FieldSources sources;
    bool         lldpSeen = false;   // protocols seen on the current link
    bool         cdpSeen  = false;

    uint32_t framesSeen = 0;
};

// Builds the screen as children of `scr`.
void ui_init(lv_obj_t *scr);
void ui_render(const UiState &s);
