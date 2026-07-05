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

## 2026-07-05 — ESP-IDF rewrite (Milestone 1) hardware gotchas

Rewriting AIDlink on ESP-IDF v5.3.5 (one codebase, targets `esp32` + `esp32s3`;
new project in `firmware-idf/`). Key things that cost time:

- **ESP-IDF install didn't include cmake/ninja** (only compilers). Fix:
  `python3 $IDF_PATH/tools/idf_tools.py install cmake ninja`.
- **NAPT Kconfig was renamed.** `CONFIG_LWIP_IP_NAPT` (old) is silently ignored on
  v5.3 → `esp_netif_napt_enable()` returns error and logs `NAPT FAILED`. Correct
  symbols: `CONFIG_LWIP_IP_FORWARD=y` **and** `CONFIG_LWIP_IPV4_NAPT=y`.
- **ESP32-S3 native-USB flashing boot-loops this board.** Flashing via the native
  USB (USB-Serial-JTAG, VID 0x303A, `/dev/cu.usbmodem101`) leaves the 2nd-stage
  bootloader crashing on entry (`rst:0x7 TG0WDT_SYS_RST`, never banners). **Stock
  `hello_world` fails identically** → it's the board/native-USB path, not our code.
  **Flash + monitor the S3 via its CH343 UART port** (VID 0x1A86,
  `/dev/cu.usbmodem5AE6043xxxx`) instead — boots fine there. The native USB is then
  free for TinyUSB NCM (M1/T5). Console = UART on the S3 for this reason.
- **Octal PSRAM deferred.** `CONFIG_SPIRAM_MODE_OCT` isn't needed for M1; left off
  (it was an early red herring while chasing the boot loop). Revisit in M5.
- Host-testable pure logic isolated in `*_util.c` (no IDF deps), tested with a
  plain `clang` assert runner — avoids the IDF linux-target/Unity machinery.

### Milestone 1 — COMPLETE (acceptance results)

All 7 tasks done on branch `esp-idf-rewrite`. Evidence:

- **Host unit tests:** `config_subnet` 5/5 PASS, `dnsfwd_remap` 6/6 PASS (plain clang).
- **Dual-target builds:** both `esp32` and `esp32s3` build clean. Classic `esp32`
  map has **0 tinyusb symbols** (USB-OTG guard verified).
- **S3 hardware (flashed via CH343 UART):** boots; SoftAP `AIDlink` up
  `172.20.1.1/26` NAPT ON; USB-NCM up `172.20.2.1/29` DHCP+NAPT; DNS forwarder
  on :53; TinyUSB driver installed — all simultaneously, no crash.
- **USB-C cable = network interface (the feature):** Mac `en12` leased
  **172.20.2.2** over the cable; ping gateway 172.20.2.1 **0% loss ~1.3 ms**;
  Mac installs **default route via the cable**. Coexists with the AP (both
  subnets served at once).
- **Not yet verifiable on the bench:** full DNS/internet-through-NAT (needs a live
  WiFi uplink + a configured STA SSID — the web UI to set it is M2; the forwarder
  correctly drops queries with no upstream). Autodetect-on-unplug (needs physical
  replug) — deferred to a live session.

### Extra gotchas found during M1

- **`idf.py set-target` + stale `build/`:** switching target without clearing
  `build/` leaves `flasher_args.json` on the old `--chip`, so `flash` fails with a
  cryptic esptool error. `flash-idf.sh` now force-cleans on target change (exact
  match, so `esp32` ≠ `esp32s3`).
- **USB-NCM RX must copy:** TinyUSB owns the rx buffer only for the callback's
  duration; `esp_netif_receive` is async → copy the frame and free it in
  `driver_free_rx_buffer`.
- **Two distinct MACs:** the S3-side netif MAC and the host-side NCM MAC must
  differ or ARP won't resolve over the cable.

## 2026-07-05 — M2 web config portal (esp_http_server) done

Ported the v9 web portal + auth to ESP-IDF. Verified end-to-end **over the USB-C
cable** (curl to the NCM gateway 172.20.2.1): login (admin/password), cookie
gating (protected 302 vs public API), full config page (7 cards / 38 fields),
/save → clamp → reboot, and **settings persist in NVS** (saved staSsid survived a
reboot and showed in /status). Both targets build; classic esp32 = 0 tinyusb.

Gotchas:
- **Chunked-encoding terminator bug:** `httpd_resp_sendstr_chunk(r, "")` writes a
  zero-length chunk = the HTTP end-of-response marker. An empty field value
  (blank staSsid) truncated the page mid-render (2 KB vs 6 KB). Fix: the chunk
  helpers skip empty strings.
- **Managed-component wedge:** after adding deps to `REQUIRES`, the build failed
  in `espressif__tinyusb` CMake (`rndis_reports.c` add_library). `rm -rf
  managed_components dependencies.lock build` + reconfigure fixed it.
- `esp_app_get_description()` needs `esp_app_desc.h`; `time/gmtime_r/strftime`
  need `<time.h>`; IDF builds with `-Werror=misleading-indentation` (no two
  `if`s on one line).
- `/status` position fields are placeholders until the M4 poller lands.

## 2026-07-05 — M3 ADBP ARINC-834 server done

Ported the ADBP feed to ESP-IDF (lwIP sockets). Split pure wire-format logic into
`adbp_frame.c` (host-tested, incl. the self-referential `length=` fixed-point
iteration) and the socket/task layer in `adbp.c`. Shared ownship state in
`pos.c` (mutex-guarded).

Verified over the USB-C cable (Python TCP client to 172.20.2.1:24000):
- `getAvionicParameters` → valid `<response errorcode="0">` with the requested
  params; unknown method → errorcode 2.
- `subscribe` (publishport 51050, 1s period) → the AID connected back to the
  Mac's port and pushed 5 frames at ~1 Hz, each framed
  `<method name="publishAvionicParameters" length="214">…` with the correct
  self-referential length.

All params read `validity="2"` (NCD) because there's no position producer yet —
that's M4 (poller/emulator writes pos_set()). Both targets build; 4 host test
suites pass; classic esp32 = 0 tinyusb.

Recurring gotcha: the `espressif__tinyusb` managed component fails CMake
reconfigure (`component.cmake:486 add_library`) whenever sources are added; fix
is `rm -rf build managed_components dependencies.lock` then rebuild. Also IDF
`-Werror=format`: uint32_t IP bytes are `unsigned long` here — cast to `(unsigned)`
for `%u`.

## 2026-07-05 — M4 position poller + sources + emulator done

Ported the poller to ESP-IDF. Pure, host-tested parsers (`poller_sources.c`,
compiled against real IDF cJSON source in the host test): Viasat nested-`value`
+ ISO8601→epoch, Panasonic flat `td_id_*` with deg×1000 sign decode. `geo.c`
(host-tested) does great-circle bearing/haversine to derive track+GS when the
source omits them. `poller.c` fetches via `esp_http_client` (insecure TLS +
`disable_auto_redirect` to mirror v9's raw-socket `setInsecure()`/no-redirect),
`apply_fix` derives+clamps, `sim_step` runs the emulator, stale→NCD watchdog.

Verified end-to-end over the cable: enabled the emulator from the web portal →
`/status` shows valid position → ADBP `getAvionicParameters` returns
`validity="1"` LAT/LON/GS/TRK/ALT/ACID/GNSS_AVAIL (track 245°→-115.00 via
norm180, ACID=F-ONEO). The full chain emulator→pos_set()→ADBP works.

Parity note: v9's emulator is a **fixed** position (fixed=true, no advance) —
matched it (dropped an initial moving-emulator version). 6 host suites pass;
both targets build; classic esp32 = 0 tinyusb.

## 2026-07-05 — M5 polish/parity + ESP-IDF rewrite COMPLETE

- **mDNS** (`services.c`): advertises `<dev_name>.local` + `_http` +
  `_aidlink-adbp` (managed component `espressif/mdns`). Boots
  `[mDNS] http://aidlink.local/`. Resolves for Wi-Fi AP clients; cable hosts use
  the fixed 172.20.2.1 gateway (mDNS doesn't bind our custom USB netif).
- **Docs**: `firmware-idf/README.md` (build/flash, cable, host tests, layout);
  main README now points to both firmwares.
- **Kept the Arduino sketch** rather than deleting it — it's the proven shipping
  build, and full parity isn't signed off until the live upstream Wi-Fi→NAT→
  internet path is confirmed on real hardware (bench has had no live uplink).
- **Deferred** (documented, low-value): the /log ring-buffer web endpoint (UART
  logging works; the IDF web UI doesn't poll /log), a cfgVer migration counter
  (per-key NVS default seeding is equivalent), and S3 16 MB partition expansion
  (the 3 MB app is 66% free).

### Final state
6 host suites pass; both targets build clean (esp32 67% / esp32s3 66% app free;
classic esp32 = 0 tinyusb symbols). On the S3, hardware-verified over the USB-C
cable: DHCP lease + NAT internet path, web portal (login/save/persist), and the
ADBP feed emitting valid ARINC-834 parameters from the emulator. The one thing
still to confirm on real hardware is the live upstream-Wi-Fi → NAT → internet
path (needs a real uplink present).
