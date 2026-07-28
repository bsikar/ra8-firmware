# tls_client

TLS client over `ra8_tls` + `ra8_net_pal` (NetX Duo transport) for the
EK-RA8D2. Closes the example-coverage gap in issue #261: neither
`ra8_tls` nor `ra8_net_pal` had a standalone example.

The board:

1. Brings up the CGC, the J-Link OB VCOM console (115200 8N1), the
   on-board RMII PHY pins + ETHA0 / RMAC0, and the RSIP-E50D engine.
2. Boots ThreadX with a single worker thread that drives NetX Duo
   (static IPv4 192.168.1.42 / 24, gateway 192.168.1.1).
3. Opens a TCP socket to a test TLS endpoint (default
   `192.168.1.1:4433`) and connects.
4. Opens an `ra8_tls` session whose BIO callbacks are bound to the NetX
   socket, then drives `ra8_tls_handshake()` to completion. Randomness
   comes from the PSA crypto layer, seeded from the RSIP TRNG through the
   `mbedtls_psa_external_get_random()` hook.
5. Sends one application record (`PING over ra8_tls\n`) and reads one
   back.
6. Prints the negotiated cipher suite and the peer-verification result:

   ```
   [tls] MTU=128 -> MSS clamp=88
   [tls] cipher=TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256 id=0xC02F verify=0x00000000 OK
   [tls] done
   ```

The example exercises the full public surface added for #261:
`ra8_tls_session_open` / `ra8_tls_handshake` / `ra8_tls_send` /
`ra8_tls_recv`, plus `ra8_tls_get_cipher_suite`,
`ra8_tls_get_verify_result`, and `ra8_tls_mss_clamp`.

## MSS clamp under the #21 MTU=128 limitation

The RA8D2 ESWM has a documented large-frame egress defect (#21), so the
whole networking stack is pinned to a 128-byte MTU. `ra8_tls_mss_clamp()`
subtracts the fixed IPv4 (20) + TCP (20) header overhead to yield an
MSS of 88, keeping every TCP segment -- TLS record bytes included --
inside one in-spec frame. NetX IP fragmentation is also enabled so any
oversized datagram is split into frames the MAC transmits cleanly.

## Build / flash

```
make -C examples/ek_ra8d2/hw_pending/tls_client            # build
make -C examples/ek_ra8d2/hw_pending/tls_client flash      # JLinkExe load
```

The per-app Makefile pins `RA8_USE_THREADX=ON`, `RA8_USE_NETXDUO=ON` and
`RA8_USE_MBEDTLS=ON`; the top-level `cmake -B build` skips this app unless
those three options are enabled, so the bare-metal default configuration
keeps building cleanly.

## Test recipe (bench)

1. Cable the EK-RA8D2 J64 RMII connector to a workstation and give that
   interface `192.168.1.1/24`.
2. Run a test TLS server on the workstation (self-signed cert is fine --
   the client reports the verification result rather than aborting):

   ```
   openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
     -days 1 -nodes -subj /CN=endpoint.test
   openssl s_server -accept 4433 -cert cert.pem -key key.pem -quiet
   ```

3. Open a serial terminal at 115200 8N1 on the J-Link OB CDC port, e.g.
   `picocom -b 115200 /dev/cu.usbmodem*`.
4. `make -C examples/ek_ra8d2/hw_pending/tls_client && \
   make -C examples/ek_ra8d2/hw_pending/tls_client flash`.
5. Reset the board and watch the `[tls] cipher=... verify=...` line.

## Why this app is hw_pending (not emulator-gated)

ra8_emulator runs the real firmware ELF, so a genuine handshake here needs
two things ra8_emulator does not yet provide:

- **A crypto-complete TLS server on the wire.** ra8_emulator's in-process
  peer (`tools/ra8_emulator/src/io/board_net.c`) is a plaintext
  Ethernet/ARP/IPv4/ICMP/TCP stack with no TLS. It can echo TCP bytes
  (that is how `threadx_netx_tcp_echo` emu-gates) but cannot complete an
  Mbed TLS handshake.
- **An entropy source under emulation.** The handshake seeds PSA from the
  RSIP TRNG, which ra8_emulator does not model (there is no
  `board_periph_rsip.c`; the RSIP register file is documented as
  vendor-invented and is routed to software crypto on silicon).

Until ra8_emulator grows a TLS-server endpoint, the transport + facade glue
is proven by the host unit test `tests/test_ra8_tls_net.c`, which drives
the same `ra8_tls` API (open -> handshake -> send -> recv -> cipher /
verify) over the `ra8_net_pal` loopback frame ring with MC/DC vectors for
every compound decision. On the bench this app runs end-to-end against a
real `openssl s_server` (recipe above).

### Emu-gate follow-up (to reach hw_validated/hil/)

A `board_net` TLS-server endpoint that makes this app emu-gate the way
`threadx_netx_tcp_echo` does. The proper, no-shortcut design:

1. Link the vendored Mbed TLS host-side into `tools/ra8_emulator` (a host
   config, not the target `port/mbedtls` config).
2. Add a TCP *server* role to `board_net` (it is a TCP client today):
   answer the firmware's SYN on port 4433, then feed the byte stream to an
   `mbedtls_ssl` server context, complete the handshake, read one record,
   and reply. A PSK cipher suite keeps the emulated handshake fast and
   certless.
3. Model the RSIP TRNG in ra8_emulator (a new `board_periph_rsip.c`) or route
   the PSA external-RNG hook to a deterministic seed under emulation so the
   handshake can draw randomness.
4. Add a `hil.conf` with a new `hil_eth_tls` mode (Pi peer on hardware,
   `board_net` peer off-target) and assert the `[tls] cipher=...` banner plus
   the peer's handshake-complete verdict.

Tracked against issue #261.
