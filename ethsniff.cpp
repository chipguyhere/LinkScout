/*
 * ethsniff.cpp - Ethernet bring-up and raw frame tap for the P486Panel.
 *
 * Board wiring (Waveshare ESP32-P4-86-Panel-ETH-2RO, built-in IP101GRI on
 * RMII):
 *
 *   RXD0=GPIO29  RXD1=GPIO30  CRS_DV=GPIO28
 *   TXD0=GPIO34  TXD1=GPIO35  TX_EN=GPIO49
 *   REF_CLK=GPIO50 (50 MHz clock in from the PHY)
 *   MDC=GPIO31   MDIO=GPIO52  RESET=GPIO51
 *
 * The RMII data pins match the ESP32-P4 Arduino defaults, so only MDC/MDIO,
 * the PHY address and the clock mode need to be passed to ETH.begin().  The
 * reset line on GPIO51 is deliberately left alone -- the PHY comes out of
 * reset on its own, and driving it isn't needed for a clean bring-up.
 *
 * Sniffing works by replacing the Ethernet driver's stack-input callback.
 * Normally esp_netif_attach() points that at the lwIP glue; we point it at
 * ourselves instead, decode anything that looks like LLDP or CDP, then hand
 * the buffer on to esp_netif_receive() exactly as the glue would have.  That
 * keeps DHCP and the rest of the IP stack working unchanged -- the tap is
 * strictly additive.
 *
 * LLDP and CDP go to L2 multicast addresses (01:80:c2:00:00:0e and
 * 01:00:0c:cc:cc:cc), neither of which is a group the IP stack ever joins, so
 * the MAC's receive filter has to be told to accept all multicast.
 */

#include <Arduino.h>
#include <ETH.h>
#include <esp_eth.h>
#include <esp_netif.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ethsniff.h"

// Board configuration.
static constexpr eth_phy_type_t   PHY_TYPE = ETH_PHY_IP101;
static constexpr int32_t          PHY_ADDR = 1;
static constexpr int              PIN_MDC  = 31;
static constexpr int              PIN_MDIO = 52;
static constexpr int              PIN_POWER = -1;            // not used on this board
static constexpr eth_clock_mode_t CLK_MODE = EMAC_CLK_EXT_IN;  // 50 MHz from PHY on GPIO50

static SemaphoreHandle_t s_lock = nullptr;
static NeighborInfo      s_slot[3];      // indexed by NeighborProto
static bool              s_pending[3] = {false, false, false};
static NeighborInfo      s_scratch;      // owned by the Ethernet RX task

static volatile uint32_t s_epoch = 1;
static volatile uint32_t s_seen = 0;
static volatile uint32_t s_decoded = 0;

// Called from the Ethernet driver's RX task for every frame the MAC accepts.
// Keep it short: decode, stash, forward.
static esp_err_t eth_rx_tap(esp_eth_handle_t hdl, uint8_t *buffer, uint32_t length, void *priv) {
    s_seen++;

    if (neighbor_parse_frame(buffer, length, &s_scratch)) {
        uint8_t idx = (uint8_t)s_scratch.proto;
        if (idx < 3) {
            s_scratch.epoch = s_epoch;
            if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
                s_slot[idx] = s_scratch;
                s_pending[idx] = true;
                xSemaphoreGive(s_lock);
                s_decoded++;
            }
            // If the UI happened to hold the lock, drop this one; the switch
            // re-advertises every 30-60 s.
        }
    }

    // Hand the frame to lwIP the way esp_eth_netif_glue would have.  This
    // transfers ownership of the buffer -- the netif frees it in every path,
    // including when the interface is down.
    return esp_netif_receive((esp_netif_t *)priv, buffer, length, nullptr);
}

bool ethsniff_begin() {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    if (!ETH.begin(PHY_TYPE, PHY_ADDR, PIN_MDC, PIN_MDIO, PIN_POWER, CLK_MODE)) {
        return false;
    }

    esp_eth_handle_t handle = ETH.handle();
    esp_netif_t *netif = ETH.netif();
    if (!handle || !netif) return false;

    // Accept LLDP/CDP multicast.  ESP-IDF 5.5 enables this by default, but say
    // so explicitly; fall back to full promiscuous mode if the MAC refuses.
    bool on = true;
    if (esp_eth_ioctl(handle, ETH_CMD_S_ALL_MULTICAST, &on) != ESP_OK) {
        if (esp_eth_ioctl(handle, ETH_CMD_S_PROMISCUOUS, &on) != ESP_OK) {
            Serial0.println("ethsniff: could not enable multicast reception");
        }
    }

    if (esp_eth_update_input_path(handle, eth_rx_tap, netif) != ESP_OK) {
        Serial0.println("ethsniff: failed to install frame tap");
        return false;
    }

    return true;
}

bool ethsniff_link_up() {
    return ETH.linkUp();
}

void ethsniff_new_link_epoch() {
    s_epoch++;
}

uint32_t ethsniff_epoch() {
    return s_epoch;
}

bool ethsniff_take(NeighborProto proto, NeighborInfo *out) {
    uint8_t idx = (uint8_t)proto;
    if (idx >= 3 || !s_lock) return false;

    bool got = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_pending[idx]) {
            *out = s_slot[idx];
            s_pending[idx] = false;
            got = true;
        }
        xSemaphoreGive(s_lock);
    }
    return got;
}

uint32_t ethsniff_frames_seen()    { return s_seen; }
uint32_t ethsniff_frames_decoded() { return s_decoded; }
