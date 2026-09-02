<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Brighton Sikarskie -->

# wifi_hal_join

The HAL-way twin of `c6_wifi_join`: same hardware, same outcome -- associate the
ESP32-C6 station with the bench access point and obtain a DHCP lease. What
differs is how the application reads.

`c6_wifi_join` drives `ra8_c6link` directly: open a link, await readiness,
register an event callback, latch the connected and disconnected events by hand,
drive the start / configure / join calls and pump the link in a wait loop. All
correct, all necessary, and all esp-hosted detail.

`wifi_hal_join` does none of that. Initialise, connect, wait for an address --
three `ra8_wifi` calls, with no link handle touched after setup, no RPC
sequence, no event callback and no interface index. The facade hides all of it
behind its vtable, with `ra8_c6link` as its first backend. Running the two apps
back to back is what shows the facade costs nothing on real hardware.

## The one piece that stays in the application

Obtaining an IP address is the IP stack's job, not the radio's, so it is a
caller-supplied provider rather than a facade call: this app wraps a NetX Duo
DHCP client and hands it to `ra8_wifi` as the address-bind hook. Keeping that
out of the facade is what lets `ra8_wifi` stay free of NetX Duo and stay
host-testable.

Like `c6_wifi_join`, this is always a credential-free image. At runtime it
prints `ra8_net_provision: READY v1`, accepts one bounded ASCII-hex provisioning
line over the debug UART, and erases the decoded record after the synchronous
network journey. The URL field is accepted for protocol parity and ignored by
this application. Received bytes are never echoed.
