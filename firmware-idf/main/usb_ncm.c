// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// USB-NCM cable networking for the ESP32-S3 (NAT/router model). Creates a custom
// esp_netif whose L2 frames are carried over TinyUSB's NCM class, gives it a
// private DHCP-served subnet, and NAPTs it to the Wi-Fi STA uplink — mirroring how
// the SoftAP clients are handled. Whole file compiles to a no-op where native USB
// isn't available.
#include "usb_ncm.h"
#include "sdkconfig.h"

#if CONFIG_SOC_USB_OTG_SUPPORTED

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/ip4_addr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "netcore.h"

static const char *TAG = "usbncm";
static esp_netif_t *s_usb_netif;   // L2-only bridge port (no IP of its own)
static uint32_t s_last_rx_ms;      // last time the host sent us a frame
static uint8_t  s_host_mac[6];     // MAC of the host-side NCM interface (the client)

typedef struct { esp_netif_driver_base_t base; } usb_driver_t;

// lwIP -> USB host: hand the frame to TinyUSB (copies into its TX, so eb=NULL).
static esp_err_t usb_transmit(void *h, void *buffer, size_t len) {
    return tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
}

// lwIP done with an RX frame we injected -> free our copy.
static void usb_free_rx(void *h, void *buffer) { free(buffer); }

// USB host -> lwIP. TinyUSB owns `buffer` only during this callback, and
// esp_netif_receive hands the frame to the async tcpip thread, so we must copy.
static esp_err_t usb_recv_cb(void *buffer, uint16_t len, void *ctx) {
    if (!s_usb_netif || len == 0) return ESP_OK;
    s_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);   // host is alive
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    if (esp_netif_receive(s_usb_netif, copy, len, copy) != ESP_OK) free(copy);
    return ESP_OK;
}

// A host is "connected" if it sent us a frame in the last 30 s (NCM hosts emit
// periodic ARP/traffic). Reports the host NCM MAC and the DHCP address the bridge
// leased it (the host is a bridge client now, not on a separate USB subnet).
bool usb_ncm_client(char *mac_str, char *ip_str) {
    if (s_last_rx_ms == 0) return false;
    if ((uint32_t)(esp_timer_get_time() / 1000) - s_last_rx_ms > 30000) return false;
    snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             s_host_mac[0], s_host_mac[1], s_host_mac[2], s_host_mac[3], s_host_mac[4], s_host_mac[5]);
    ip_str[0] = 0;
    esp_netif_t *br = netcore_bridge_netif();
    esp_netif_pair_mac_ip_t pair = {0};
    memcpy(pair.mac, s_host_mac, 6);
    if (br && esp_netif_dhcps_get_clients_by_mac(br, 1, &pair) == ESP_OK && pair.ip.addr)
        snprintf(ip_str, 16, IPSTR, IP2STR(&pair.ip));
    else
        strlcpy(ip_str, "(pending)", 16);
    return true;
}

static void usb_free_tx(void *buffer, void *ctx) { (void)buffer; (void)ctx; }

static esp_err_t usb_post_attach(esp_netif_t *netif, void *args) {
    usb_driver_t *d = args;
    d->base.netif = netif;
    esp_netif_driver_ifconfig_t ifcfg = {
        .handle = d,
        .transmit = usb_transmit,
        .driver_free_rx_buffer = usb_free_rx,
    };
    return esp_netif_set_driver_config(netif, &ifcfg);
}

void usb_ncm_start(const aidlink_cfg_t *c) {
    (void)c;
    // The bridge (built by netcore_start) owns the AID IP, DHCP pool and NAPT. The
    // USB link is just another L2 port on it, so both Wi-Fi and cable clients land
    // on the same 172.20.1.0/26 subnet and reach the AID at 172.20.1.1.
    esp_netif_t *br = netcore_bridge_netif();
    if (!br) { ESP_LOGE(TAG, "[USB] no bridge netif; USB-NCM not started"); return; }

    // --- L2-only esp_netif over an ethernet netstack: no IP/ARP/DHCP of its own
    //     (flags=0, ip_info=NULL), it gets bridged below. ---
    esp_netif_inherent_config_t base = {
        .flags = 0,
        .ip_info = NULL,
        .if_key = "USB_NCM",
        .if_desc = "usb",
        .route_prio = 15,
    };
    esp_netif_config_t cfg = {
        .base = &base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_usb_netif = esp_netif_new(&cfg);
    if (!s_usb_netif) { ESP_LOGE(TAG, "esp_netif_new failed"); return; }

    // port (device-side) MAC — distinct from the host-side NCM MAC.
    uint8_t nmac[6];
    esp_read_mac(nmac, ESP_MAC_ETH);
    nmac[0] |= 0x02;   // locally administered
    nmac[5] ^= 0x01;   // distinct from the host MAC below
    esp_netif_set_mac(s_usb_netif, nmac);

    usb_driver_t *drv = calloc(1, sizeof(*drv));
    drv->base.post_attach = usb_post_attach;
    ESP_ERROR_CHECK(esp_netif_attach(s_usb_netif, drv));

    // Start the port (registers its lwip_netif), then add it to the already-started
    // bridge. Both must be started before esp_netif_bridge_add_port().
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_usb_netif, NULL, 0, NULL);
    ESP_ERROR_CHECK(esp_netif_bridge_add_port(br, s_usb_netif));

    // --- TinyUSB + NCM class ---
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_recv_cb,
        .free_tx_buffer = usb_free_tx,
        .user_context = NULL,
    };
    esp_read_mac(net_cfg.mac_addr, ESP_MAC_ETH);   // host-side NCM MAC (distinct from nmac)
    memcpy(s_host_mac, net_cfg.mac_addr, 6);        // remember it for the clients list
    ESP_ERROR_CHECK(tinyusb_net_init(&net_cfg));

    ESP_LOGI(TAG, "[USB] NCM bridged onto the AID subnet (DHCP+NAPT via bridge)");
}

// Detach from the USB host (uninstall TinyUSB) so the port disconnects cleanly.
// Used by /dfu: rebooting into the ROM downloader while the host still holds
// the NCM device intermittently leaves the host with a stale device and the
// downloader's USB-Serial-JTAG never enumerates — a physical BOOT+RST was the
// only recovery. A real detach first lets the downloader appear reliably.
void usb_ncm_stop(void) {
    tinyusb_driver_uninstall();
}

#else  // no native USB (e.g. classic ESP32)
void usb_ncm_start(const aidlink_cfg_t *c) { (void)c; }
void usb_ncm_stop(void) {}
bool usb_ncm_client(char *mac_str, char *ip_str) { (void)mac_str; (void)ip_str; return false; }
#endif
