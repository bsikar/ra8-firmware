<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Brighton Sikarskie -->

# wifi_hal_join -- the same Wi-Fi join, through the `ra8_wifi` facade

This is the HAL-way twin of [`c6_wifi_join`](../c6_wifi_join/). Both do the same
thing on the same hardware: associate the ESP32-C6 station with the bench access
point and obtain a DHCP lease. The difference is *how the application reads*.

`c6_wifi_join` talks to the co-processor through `ra8_c6link` directly: it opens
a link, calls `ra8_c6link_await_ready`, registers an event callback, latches the
station-connected/disconnected events by hand, drives `ra8_c6link_wifi_start` /
`ra8_c6link_sta_cfg_set` / `ra8_c6link_wifi_join`, and pumps `ra8_c6link_poll` in
a wait loop -- all correct, all necessary, and all esp-hosted detail.

`wifi_hal_join` does none of that. The entire network journey is:

```c
ra8_wifi_cfg_t cfg = {};
ra8_wifi_c6link_setup(&s_c6, &bcfg, &cfg);   // pick the C6 backend
cfg.ip_bind = wifi_hal_ip_bind;              // NetX DHCP provider
cfg.ip_ctx  = &s_link;

ra8_wifi_init(&s_wifi, &cfg);                // radio + link up, ready
ra8_wifi_connect(&s_wifi, ssid, psk);        // join, blocking to associated
ra8_wifi_lease_t lease = {};
ra8_wifi_wait_ip(&s_wifi, &lease);           // DHCP -- lease.ip is the address
```

There is no `ra8_c6link` handle poked after setup, no RPC sequence, no event
callback, no `wifi_mode_t`, no interface index. The facade hides all of it
behind the vtable in `libs/ra8_wifi`, with `ra8_c6link` as its first backend.

## What stays in the application

Obtaining an IP is the IP stack's job, not the radio's, so it is a
caller-supplied provider rather than a facade call: `wifi_hal_ip_bind`
(`src/wifi_hal_ip.c`) wraps a NetX Duo DHCP client and is handed to the facade
as `cfg.ip_bind`. That is the one deliberately stack-specific piece -- keeping it
out of `ra8_wifi` is what lets the facade stay free of NetX Duo and stay
host-testable.

## Build and run

```sh
# credentials from the environment (or coprocessor/esp32c6/wifi.env)
RA8_C6_WIFI_SSID=ra8-bench RA8_C6_WIFI_PSK="..." make
make flash
```

Built without credentials the image still links; it prints
`no Wi-Fi credentials compiled in` and stops, so nothing secret is ever
committed.

## Bench setup

Identical to `c6_wifi_join`: the C6 harness on **J26**, board switches
**SW4 1=OFF 2=OFF 3=ON 4=OFF**. Run under `make hil-c6` (the `c6` tier), not the
default HIL pass. The PASS line is:

```
wifi_hal: PASS ra8_wifi joined the bench Wi-Fi and DHCP leased an address
```
