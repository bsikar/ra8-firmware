# ethernet_http_responder

Minimal bare-metal HTTP/1.1 GET responder on the EK-RA8D2's on-chip
Ethernet MAC. Shares the descriptor-ring path (`ra_eth`, `ra_etha`,
`ra_eth_gwca`, `ra_rmac`) with `ethernet_tcp_echo` but instead of
echoing the inbound payload it returns a fixed `HTTP/1.1 200 OK` body
on the first request segment, then closes the connection.

## What it does

After `ra_cgc_init()` and pin mux, the firmware:

1. Routes the RGMII pins via `ra_board_ethernet_init()`.
2. Opens the NIC at MAC `02:00:00:00:00:01`.
3. Polls the PHY's BMSR until link-up.
4. Adopts the static address `192.168.1.44 / 255.255.255.0`.
5. Drops into a poll loop:
   - **ARP** -- replies to "who-has 192.168.1.44".
   - **ICMP echo** -- mirrors the payload back.
   - **TCP** on port 80: accepts the SYN, on the first inbound data
     segment sends a canned `HTTP/1.1 200 OK` with body
     `Hello from RA8D2!`, then immediately FINs the connection.
6. SCI8 prints the boot banner `eth: ip=192.168.1.44 port=80 proto=http`
   followed by `eth: ready` so HIL automation can discover the IP.

The body is short and deterministic so HIL tests assert by simple
string match (look for `Hello from RA8D2` in the response).

## How to test (manually)

1. `make build` then `make flash`.
2. Wire the EK-RA8D2 RJ45 to a host with a static IPv4 address in
   `192.168.1.0/24` (e.g. `192.168.1.1`).
3. `curl -v http://192.168.1.44/` -- expect `HTTP/1.1 200 OK` and the
   body `Hello from RA8D2!`.

## Status

HIL-validated end to end via `scripts/hil_eth_tcp.sh` (HTTP mode).
