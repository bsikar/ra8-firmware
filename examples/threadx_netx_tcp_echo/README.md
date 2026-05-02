# threadx_netx_tcp_echo

ThreadX + NetX Duo TCP echo demo for the EK-RA8D2.

## What it does

Brings up SCI8 (115200 8N1, J-Link OB CDC), routes the eleven RMII pins on
J64 to the on-chip Ethernet MAC, then enters ThreadX. NetX Duo runs on top
of the hardware Ethernet driver shim under `port/netxduo/` and listens on
**TCP port 7** at the static IPv4 address **192.168.1.42 / 24**.

Every received segment is echoed back unchanged. The application logs
`[netx] echoed N bytes from a.b.c.d` to SCI8 once per receive.

This is the same hardware test as `examples/ethernet_tcp_echo` (which
uses the hand-rolled `ra_net` stack) -- this app is the NetX Duo
replacement.

## Test recipe

1. Wire the EK-RA8D2 J64 RMII port to a host PC's Ethernet jack (direct
   crossover or through a switch).
2. Configure the host's NIC for `192.168.1.x / 24` (any address other
   than `.42`).
3. Flash and reset:
   ```
   make flash
   ```
4. Open the J-Link OB CDC port at 115200 8N1 to see the boot banner and
   echo log.
5. From the host:
   ```
   nc 192.168.1.42 7
   ```
   Anything you type comes back. The board's UART log will show
   `[netx] echoed N bytes from <host-ip>` for each line.

## Crypto ALT note

The build also pulls in `port/netxduo/nx_crypto_aes_alt.c` and
`port/netxduo/nx_crypto_sha256_alt.c`. These are linked in via
`-Wl,--wrap=...` so NetX Crypto's `_nx_crypto_method_aes_*` and
`_nx_crypto_method_sha256_*` algorithm-table entries route their
init / operation / cleanup callbacks through `ra_rsip_aes_cipher`
and `ra_rsip_sha256` on real silicon. The TCP echo path doesn't use
crypto, but the same target is the proving ground for adding NetX
Secure (TLS) on top later.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED init/toggle (per UM Table 24 p
31) and `ra_board_ethernet_init` for the on-board PEF7071 PHY (RGMII
per UM Table 26 p 33). SCI8 console pins follow UM Table 13 p 24.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Section 6.1 + Tables 13 p 24 / 24 p 31 / 26 p 33, and HUM
(R01UH1065EJ0130) Ethernet + RSIP chapters.
