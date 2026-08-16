# libs/ra8_wifi -- ra8_wifi vs ra8_c6link

`ra8_c6link` owns the ESP32-C6 wire and nothing else. `ra8_wifi` is a
radio-agnostic station facade over an `ra8_wifi_backend_t` vtable, of which the
C6 is currently the only implementation (`ra8_wifi_c6link_setup()`).

The facade is not a rename of the driver. It adds the parts that are *not* about
the radio: an association state machine (`k_ra8_wifi_state_down` through
`k_ra8_wifi_state_ip_bound`), a bounded join wait, and an IP-lease
step (`ra8_wifi_wait_ip()`) driven by a caller-supplied bind function -- DHCP
belongs to the IP stack, so it is deliberately not a backend row.

| | `ra8_wifi` | `ra8_c6link` |
|---|---|---|
| Owns | station state machine, join timeout, IP-lease step | payload header, TLV envelope, protobuf RPC, transaction pump |
| Knows about the C6 | no (vtable only) | entirely |
| Call it when | you want "join this network" and do not care what radio | you need something the facade does not expose |

**Which do I use?** `ra8_wifi`, unless you need a C6 capability the vtable does
not carry. Either way the app still binds the transport itself
(`ra8_esp_hosted_c6link_bind()` from `port/esp-hosted/`); the facade abstracts
the radio, not the wiring.

<!-- disambig
this: libs/ra8_wifi
that: libs/ra8_c6link
symbol: ra8_wifi_backend_t
symbol: ra8_wifi_c6link_setup
symbol: ra8_wifi_wait_ip
symbol: k_ra8_wifi_state_ip_bound
symbol: ra8_c6link_wifi_join
users: ra8_wifi = 1
users: ra8_c6link = 5
-->
