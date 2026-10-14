/*
 * ethsniff.h - Ethernet bring-up plus a raw-frame tap for LLDP/CDP.
 *
 * Brings up the P486Panel's built-in IP101GRI PHY, requests a DHCP lease as
 * normal, and additionally intercepts every received frame on its way to the
 * IP stack so link-layer discovery frames can be decoded.
 */

#pragma once

#include <stdint.h>
#include "neighbor.h"

// Starts Ethernet (DHCP) and installs the raw frame tap.  Returns false if
// ETH.begin() failed.
bool ethsniff_begin();

// True while the PHY reports link.
bool ethsniff_link_up();

// Bumps the link generation counter.  Frames are stamped with the generation
// they arrived on, which is how the UI knows whether what it's showing predates
// the current link.
void ethsniff_new_link_epoch();
uint32_t ethsniff_epoch();

// Copies the newest frame decoded for `proto` and returns true, but only if one
// has arrived since the last successful take.  Safe to call from the LVGL loop.
bool ethsniff_take(NeighborProto proto, NeighborInfo *out);

// Total frames seen by the tap, and LLDP/CDP frames decoded.  Diagnostics only.
uint32_t ethsniff_frames_seen();
uint32_t ethsniff_frames_decoded();
