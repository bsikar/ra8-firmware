# ethernet_udp_echo

Bare-metal ARP / ICMP / UDP echo responder on the EK-RA8D2's on-chip
Ethernet MAC. Companion to `ethernet_tcp_echo`, sharing the same
descriptor-ring path (`ra_eth`, `ra_etha`, `ra_eth_gwca`, `ra_rmac`)
but answering datagrams instead of streams. No lwIP -- every frame is
built and parsed byte-for-byte against the RFCs.

## What it does

After `ra_cgc_init()` and pin mux, the firmware:

1. Routes the RGMII pins to the on-chip MAC via `ra_board_ethernet_init()`.
2. Opens the NIC at MAC `02:00:00:00:00:01` (locally administered).
3. Polls the PHY's BMSR until link-up.
4. Adopts the static address `192.168.1.43 / 255.255.255.0`.
5. Drops into a poll loop:
   - **ARP** (RFC 826) -- replies to "who-has 192.168.1.43".
   - **ICMP echo** (RFC 792) -- mirrors the payload back.
   - **UDP echo** (RFC 768 + RFC 862) on port 7 -- mirrors the entire
     datagram payload back to the sender with src/dst port swapped and
     the UDP + IPv4 checksums recomputed.
6. **LED1** toggles on every frame received.
7. **LED2** toggles on every frame transmitted.
8. SCI8 @ 115200 prints the boot banner `eth: ip=192.168.1.43 port=7
   proto=udp` followed by `eth: ready` so HIL automation can discover
   the IP and start probing.

## How to test (manually)

1. `make build` then `make flash`.
2. Wire the EK-RA8D2 RJ45 to a host with a static IPv4 address in
   `192.168.1.0/24` (e.g. `192.168.1.1`).
3. `ping 192.168.1.43` -- ICMP echo replies at ~1 ms.
4. `echo hello | nc -u -q1 192.168.1.43 7` -- the firmware mirrors the
   bytes back; `nc -u` prints them.

## Status

HIL-validated end to end via `scripts/hil_eth_tcp.sh` (UDP mode). Hand-
written checksum and frame builders are exercised through the host
unit tests (`tests/test_ra_net_udp.c`).
