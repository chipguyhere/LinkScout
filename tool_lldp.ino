/*
 * tool_lldp.ino - the Switch Port Finder tool, from lldp_display.
 *
 * Listens on the built-in Ethernet for LLDP (IEEE 802.1AB) and Cisco CDP and
 * shows switch name, port name, VLAN and management address, plus the DHCP
 * lease and a live link indicator.  See ethsniff.cpp for how frames are tapped
 * without disturbing the IP stack.
 *
 * Staleness: everything the network told us goes grey when the link drops,
 * because it describes a link that no longer exists.  Each panel returns to
 * full colour only when that fact is re-learned on the current link -- a new
 * LLDP/CDP advertisement, or a new DHCP lease.
 */

// How often we re-read link state and repaint.  The UI only touches widgets
// whose content changed, so an idle screen doesn't redraw.
static const uint32_t POLL_INTERVAL_MS = 200;

// The PHY takes a couple of seconds to reset and autonegotiate after
// ETH.begin(), and the driver reports no link the whole time.  Calling that
// LINK DOWN is a misleading diagnosis of a fault that hasn't happened -- so
// until the link comes up, or this window runs out, the band says we're still
// starting.  The grace applies only before the first link-up: once we've seen
// a link, a later drop is a real one and gets reported immediately.
//
// Measured from when the tool starts, not from boot: the menu may have sat on
// screen for any length of time before Ethernet was brought up.
static const uint32_t LINK_GRACE_MS = 8000;

static UiState ui;

// Latest advertisement from each protocol, kept separately so a switch running
// both doesn't make the display flap between them.
static NeighborInfo s_lldp, s_cdp;
static bool s_haveLldp = false, s_haveCdp = false;

static bool     s_lastLinkUp = false;
static bool     s_everLinkedUp = false;   // ends the grace window for good
static uint32_t s_toolStartMs = 0;        // grace window starts here
static uint32_t s_ipEpoch = 0;        // link generation the current lease was issued on
static volatile bool s_gotIpEvent = false;

// DHCP completion is an event rather than a polled value: on a link bounce the
// switch may well hand back the same address, and re-reading localIP() can't
// tell "still the old lease" from "freshly renewed".
static void onEthGotIp(arduino_event_id_t event) {
    s_gotIpEvent = true;
}

// True when this advertisement was captured on the link we have right now.
static bool is_current(const NeighborInfo &n) {
    return n.epoch == ethsniff_epoch();
}

// Picks the advertisement to display and fills any gaps in it from the other
// protocol.  LLDP wins ties -- its port naming tends to be the more precise of
// the two -- but CDP contributes the native VLAN and platform string that LLDP
// often leaves out.
static bool build_neighbor(NeighborInfo *out, FieldSources *src,
                           bool *lldpSeen, bool *cdpSeen) {
    bool lldpFresh = s_haveLldp && is_current(s_lldp);
    bool cdpFresh  = s_haveCdp  && is_current(s_cdp);
    *lldpSeen = lldpFresh;
    *cdpSeen  = cdpFresh;

    const NeighborInfo *primary = nullptr, *secondary = nullptr;
    if (lldpFresh || cdpFresh) {
        // At least one is current: never fall back to something older.
        primary   = lldpFresh ? &s_lldp : &s_cdp;
        secondary = lldpFresh ? (cdpFresh ? &s_cdp : nullptr) : nullptr;
    } else if (s_haveLldp || s_haveCdp) {
        primary   = s_haveLldp ? &s_lldp : &s_cdp;
        secondary = (s_haveLldp && s_haveCdp) ? &s_cdp : nullptr;
    } else {
        return false;
    }

    *out = *primary;

    // Every field starts out attributed to the primary protocol; anything
    // filled in from the secondary below gets re-attributed as we go, so the
    // on-screen captions always name the protocol that really supplied the
    // value sitting next to them.
    NeighborProto p = primary->proto;
    src->device = src->port = src->portDescr = src->platform = p;
    src->software = src->mgmt = src->vlan = src->duplex = p;

    // Only merge when both frames came from the same port -- same source MAC,
    // same link generation.  Otherwise they're not describing the same thing.
    if (secondary && secondary->epoch == primary->epoch &&
        strcmp(secondary->srcMac, primary->srcMac) == 0) {
        NeighborProto q = secondary->proto;
        if (!out->device[0])    { strcpy(out->device, secondary->device);       src->device = q; }
        if (!out->port[0])      { strcpy(out->port, secondary->port);           src->port = q; }
        if (!out->portDescr[0]) { strcpy(out->portDescr, secondary->portDescr); src->portDescr = q; }
        if (!out->platform[0])  { strcpy(out->platform, secondary->platform);   src->platform = q; }
        if (!out->software[0])  { strcpy(out->software, secondary->software);   src->software = q; }
        if (!out->mgmt[0])      { strcpy(out->mgmt, secondary->mgmt);           src->mgmt = q; }
        if (!out->duplex[0])    { strcpy(out->duplex, secondary->duplex);       src->duplex = q; }
        if (!out->vlan)         { out->vlan = secondary->vlan;                  src->vlan = q; }
    }
    return true;
}

// Builds the screen in its STARTING ETHERNET state.  Cheap; the caller paints
// it before calling lldp_tool_start_network().
void lldp_tool_begin(lv_obj_t *scr) {
    s_toolStartMs = millis();

    ui_init(scr);
    ui.linkStarting = true; // the very first paint is inside the grace window
    ui_render(ui);
}

// Brings up Ethernet and the frame tap.  ETH.begin() blocks for about 4 s
// (measured: PHY reset and the driver's start-up), which is why the screen has
// to be on the panel before this runs.
void lldp_tool_start_network() {
    Network.onEvent(onEthGotIp, ARDUINO_EVENT_ETH_GOT_IP);

    if (!ethsniff_begin()) {
        Serial0.println("Ethernet init failed");
    }

    ETH.macAddress().toCharArray(ui.mac, sizeof(ui.mac));
    Serial0.printf("Switch Port Finder: MAC %s -- listening for LLDP and CDP\n", ui.mac);
}

void lldp_tool_poll() {
    static uint32_t lastPoll = 0;

    uint32_t now = millis();
    if (now - lastPoll < POLL_INTERVAL_MS) return;
    lastPoll = now;

    // A link transition invalidates everything we learned before it, in both
    // directions: the old facts describe a link that has gone away, and a new
    // link hasn't told us anything yet.  Bumping the generation counter on
    // either edge marks all stored data as belonging to a previous link, and it
    // stays marked until fresh frames arrive.
    bool linkUp = ethsniff_link_up();
    if (linkUp != s_lastLinkUp) {
        s_lastLinkUp = linkUp;
        ethsniff_new_link_epoch();
        Serial0.printf("Link %s\n", linkUp ? "up" : "down");
    }

    if (s_gotIpEvent) {
        s_gotIpEvent = false;
        s_ipEpoch = ethsniff_epoch();
        Serial0.printf("DHCP lease: %s\n", ETH.localIP().toString().c_str());
    }

    NeighborInfo n;
    if (ethsniff_take(NEIGHBOR_LLDP, &n)) {
        s_lldp = n;
        s_haveLldp = true;
        Serial0.printf("LLDP: %s port %s\n", n.device, neighbor_port_label(n));
    }
    if (ethsniff_take(NEIGHBOR_CDP, &n)) {
        s_cdp = n;
        s_haveCdp = true;
        Serial0.printf("CDP: %s port %s\n", n.device, neighbor_port_label(n));
    }

    if (linkUp) s_everLinkedUp = true;

    ui.linkUp       = linkUp;
    ui.linkStarting = !linkUp && !s_everLinkedUp && (now - s_toolStartMs) < LINK_GRACE_MS;
    ui.linkSpeed    = ETH.linkSpeed();
    ui.fullDuplex   = ETH.fullDuplex();
    ui.framesSeen   = ethsniff_frames_seen();

    // Once we've had a lease we keep showing it even after the interface drops
    // it -- greyed out rather than blanked, so the last known address stays
    // readable.
    if (ETH.hasIP()) {
        ui.haveIp = true;
        ETH.localIP().toString().toCharArray(ui.ip, sizeof(ui.ip));
    }
    ui.ipStale = !linkUp || s_ipEpoch != ethsniff_epoch();

    ui.haveNeighbor = build_neighbor(&ui.neighbor, &ui.sources,
                                     &ui.lldpSeen, &ui.cdpSeen);
    ui.neighborStale = ui.haveNeighbor && (!linkUp || !is_current(ui.neighbor));

    ui_render(ui);
}
