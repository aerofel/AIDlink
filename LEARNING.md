# AIDlink — Learning Journal

## 2026-07-05 — AP-client DNS black-hole (intermittent DNS "not forwarded")

- **Symptom:** Devices on the AIDlink AP (esp. iPhone/iPad reaching the Viasat
  endpoint) intermittently failed DNS resolution.
- **Root cause (confirmed on hardware):** `startAP()` handed clients a DNS server
  via DHCP that was wrong/dead depending on uplink timing. When the STA uplink was
  not yet associated at AP-start, `WiFi.dnsIP()==0` and the code fell back to
  `dns=ip` → advertised **172.20.1.1** as the resolver, but the firmware ran **no
  DNS server** there. Clients cache the DHCP-provided resolver for the whole lease
  (**default 120 min**), so DNS stayed dead even after the uplink recovered. iOS is
  especially aggressive about caching the leased resolver.
- **Fix:** Added a small on-device UDP **DNS forwarder** (listens on AP `:53`,
  relays each query to the *live* `WiFi.dnsIP()` / `apClientDns`, NAT-style txn-id
  remap in `dnsPend[]`). `startAP()` now always offers **172.20.1.1** as the client
  DNS. Because the upstream is resolved per-query (not baked into the lease), DNS
  follows STA reconnects and uplink-DNS changes and never goes stale.
- **Also:** re-assert the DHCP DNS-offer (`OFFER_DNS` /
  `ESP_NETIF_DOMAIN_NAME_SERVER`) inside `applyDhcpPool()` after its
  `dhcps_stop()/start()`, so the offer can't be dropped by that restart.

### ESP32 Arduino core 3.3.10 gotchas (WiFi/NetworkInterface)
- `WiFi.softAPConfig(ip,gw,mask,lease,dns)` → `NetworkInterface::config(local,gw,
  subnet,dns1,dns2,dns3)` with **dns1=lease-start, dns2=dns**. The DHCP DNS handed
  to clients comes from **dns2**, and `OFFER_DNS` is only enabled when `dns2 != 0`.
- The device's *own* resolver (Viasat poll, NTP) uses the **STA** netif DNS, not the
  AP netif — setting AP-netif DNS MAIN to 172.20.1.1 for the DHCP offer does not
  affect the device's outbound lookups.
- Bench note: `tools/flash.sh` piped to `tail` can report a non-zero exit while the
  upload actually succeeds; verify with the boot banner version over serial.

### Verification status
- Structural fix confirmed on hardware (forwarder up on :53; clients offered
  172.20.1.1; correct `(uplink down)` reporting). **Live positive-path resolution
  still to be confirmed with the real Viasat/Aircalin uplink present** — with the
  uplink up, a client can test: `nslookup wifi.inflight.viasat.com 172.20.1.1`.
