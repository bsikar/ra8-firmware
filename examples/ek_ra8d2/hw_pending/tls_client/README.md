# tls_client

TLS client over `ra8_tls` plus `ra8_net_pal` (NetX Duo transport), closing the
example-coverage gap in #261 -- neither library had a standalone example.

The board brings up the console, the on-board RMII PHY pins with ETHA0/RMAC0 and
the RSIP engine, boots ThreadX with a single worker driving NetX Duo, opens a TCP
socket to a test TLS endpoint, then opens an `ra8_tls` session whose BIO
callbacks are bound to the NetX socket and drives the handshake to completion.
Randomness comes from the PSA crypto layer, seeded from the RSIP TRNG through the
`mbedtls_psa_external_get_random()` hook. It then sends one application record,
reads one back, and prints the negotiated cipher suite and the peer-verification
result -- **reported, not fatal**, so a self-signed server certificate is fine.

That covers the whole surface added for #261: session open, handshake, send,
recv, plus cipher-suite, verify-result and MSS-clamp queries.

## MSS clamp under the #21 MTU=128 limitation

The RA8D2 ESWM has a documented large-frame egress defect (#21), so the whole
networking stack is pinned to a 128-byte MTU. `ra8_tls_mss_clamp()` subtracts the
fixed IPv4 and TCP header overhead to yield an MSS that keeps every TCP segment
-- TLS record bytes included -- inside one in-spec frame. NetX IP fragmentation is
also enabled, so any oversized datagram is split into frames the MAC transmits
cleanly.

The per-app Makefile pins the ThreadX, NetX Duo and Mbed TLS options on, and the
top-level configure **skips this app** unless all three are enabled -- which is
what keeps the bare-metal default configuration building cleanly.

## Blocked on

Two things nothing off-target can supply, so this app cannot be gated by an
emulator run the way `threadx_netx_tcp_echo` is:

- **A crypto-complete TLS server on the wire.** The in-process peer is a
  plaintext Ethernet/ARP/IPv4/ICMP/TCP stack with no TLS. It can echo TCP bytes,
  but it cannot complete an Mbed TLS handshake.
- **An entropy source under emulation.** The handshake seeds PSA from the RSIP
  TRNG, which is not modelled -- the RSIP register file is vendor-invented and is
  routed to software crypto on silicon.

Closing that needs a TLS-server role in the emulated network peer plus a modelled
or deterministic entropy source, tracked in #261. Meanwhile the transport and
facade glue are proven by `tests/test_ra8_tls_net.c`, which drives the same API
over the `ra8_net_pal` loopback frame ring with MC/DC vectors for every compound
decision, and on the bench the app runs end to end against a real
`openssl s_server`.
