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
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "freertos/FreeRTOS.h"
#include "dhcpserver/dhcpserver.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "netcore.h"

static const char *TAG = "usbncm";
static esp_netif_t *s_usb_netif;
static esp_netif_ip_info_t s_ip;

typedef struct { esp_netif_driver_base_t base; } usb_driver_t;

static inline uint32_t mk_netaddr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

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
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    if (esp_netif_receive(s_usb_netif, copy, len, copy) != ESP_OK) free(copy);
    return ESP_OK;
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
    // --- static IP for the USB subnet (e.g. 172.20.2.1/29) ---
    memset(&s_ip, 0, sizeof(s_ip));
    esp_netif_set_ip4_addr(&s_ip.ip, c->usb_ip[0], c->usb_ip[1], c->usb_ip[2], c->usb_ip[3]);
    esp_netif_set_ip4_addr(&s_ip.gw, c->usb_ip[0], c->usb_ip[1], c->usb_ip[2], c->usb_ip[3]);
    uint32_t m = cfg_netmask_from_prefix(c->usb_prefix);
    esp_netif_set_ip4_addr(&s_ip.netmask, (m >> 24) & 0xFF, (m >> 16) & 0xFF, (m >> 8) & 0xFF, m & 0xFF);

    // --- create a DHCP-server esp_netif over an ethernet netstack ---
    esp_netif_inherent_config_t base = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &s_ip,
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

    // netif (S3-side) MAC — must differ from the host-side NCM MAC for ARP to work.
    uint8_t nmac[6];
    esp_read_mac(nmac, ESP_MAC_ETH);
    nmac[0] |= 0x02;   // locally administered
    nmac[5] ^= 0x01;   // distinct from the host MAC below
    esp_netif_set_mac(s_usb_netif, nmac);

    usb_driver_t *drv = calloc(1, sizeof(*drv));
    drv->base.post_attach = usb_post_attach;
    ESP_ERROR_CHECK(esp_netif_attach(s_usb_netif, drv));

    // bring the interface up (no real link events on this virtual netif)
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_usb_netif, NULL, 0, NULL);

    // --- DHCP pool + DNS offer (USB gateway IP -> served by the DNS forwarder) ---
    esp_netif_dhcps_stop(s_usb_netif);
    esp_netif_set_ip_info(s_usb_netif, &s_ip);
    dhcps_lease_t lease = {0};
    lease.enable = true;
    lease.start_ip.addr = mk_netaddr(c->usb_ip[0], c->usb_ip[1], c->usb_ip[2], c->usb_ip[3] + 1);
    lease.end_ip.addr = mk_netaddr(c->usb_ip[0], c->usb_ip[1], c->usb_ip[2], c->usb_ip[3] + 4);
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    esp_netif_dns_info_t di = {0};
    di.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_ip4_addr(&di.ip.u_addr.ip4, c->usb_ip[0], c->usb_ip[1], c->usb_ip[2], c->usb_ip[3]);
    esp_netif_set_dns_info(s_usb_netif, ESP_NETIF_DNS_MAIN, &di);
    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dhcps_start(s_usb_netif);

    if (c->napt_enable) {
        esp_err_t e = esp_netif_napt_enable(s_usb_netif);
        ESP_LOGI(TAG, "[USB] NAPT %s", e == ESP_OK ? "ON" : "FAILED");
    }

    // --- TinyUSB + NCM class ---
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = usb_recv_cb,
        .free_tx_buffer = usb_free_tx,
        .user_context = NULL,
    };
    esp_read_mac(net_cfg.mac_addr, ESP_MAC_ETH);   // host-side NCM MAC (distinct from nmac)
    ESP_ERROR_CHECK(tinyusb_net_init(&net_cfg));

    // The DNS forwarder already binds 0.0.0.0:53, so it serves the USB gateway too.
    ESP_LOGI(TAG, "[USB] NCM up: %d.%d.%d.%d/%d DHCP+NAPT, DNS->self",
             c->usb_ip[0], c->usb_ip[1], c->usb_ip[2], c->usb_ip[3], c->usb_prefix);
}

#else  // no native USB (e.g. classic ESP32)
void usb_ncm_start(const aidlink_cfg_t *c) { (void)c; }
#endif
