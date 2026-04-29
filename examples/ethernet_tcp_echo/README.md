# ethernet_tcp_echo

Bare-metal ARP / ICMP / TCP echo responder on the EK-RA8D2's on-chip
Ethernet MAC. Exercises the descriptor-ring TX/RX path landed in
sweep 2 (`ra_eth`, `ra_etha`, `ra_eth_gwca`, `ra_rmac`). No lwIP --
every frame is built and parsed byte-for-byte against the RFCs.

## What it does

After `ra_cgc_init()` and pin mux, the firmware:

1. Routes the eleven J64 RMII pins to the on-chip MAC (PSEL = 0x11).
2. Opens the NIC at MAC `02:00:00:00:00:01` (locally administered).
3. Polls the PHY's BMSR until link-up.
4. Adopts the static address `192.168.1.42 / 255.255.255.0`,
   gateway `192.168.1.1`.
5. Drops into a poll loop:
   - **ARP** (RFC 826) -- replies to "who-has 192.168.1.42".
   - **ICMP echo** (RFC 792) -- mirrors the payload back.
   - **TCP** (RFC 793) on port 7 -- single-connection FSM:
     `LISTEN -> SYN-RECEIVED -> ESTABLISHED -> CLOSE-WAIT -> LAST-ACK`.
6. **LED1** toggles on every frame received.
7. **LED2** toggles on every frame transmitted.
8. SCI8 @ 115200 logs `ra8d2: link up`, ARP / TCP-connect / echo lines
   on the on-board J-Link OB CDC bridge (PD_02 / PD_03).

Checksums are recomputed from scratch (IPv4 header, ICMP, TCP
pseudo-header), so packet captures hash-validate cleanly in Wireshark.

## Pinout (EK-RA8D2 v1, J64 RMII connector)

| Net          | Pin    | PSEL                        | Direction      |
|--------------|--------|-----------------------------|----------------|
| ETH_REF_CLK  | P7_00  | k_ra_psel_ether_rmii (0x11) | 50 MHz REFCLK  |
| ETH_MDC      | P4_01  | k_ra_psel_ether_rmii (0x11) | MDIO clock     |
| ETH_MDIO     | P4_02  | k_ra_psel_ether_rmii (0x11) | MDIO data      |
| ETH_TXD0     | P7_01  | k_ra_psel_ether_rmii (0x11) | Tx data 0      |
| ETH_TXD1     | P7_02  | k_ra_psel_ether_rmii (0x11) | Tx data 1      |
| ETH_TX_EN    | P7_03  | k_ra_psel_ether_rmii (0x11) | Tx enable      |
| ETH_RXD0     | P7_04  | k_ra_psel_ether_rmii (0x11) | Rx data 0      |
| ETH_RXD1     | P7_05  | k_ra_psel_ether_rmii (0x11) | Rx data 1      |
| ETH_RX_DV    | P7_06  | k_ra_psel_ether_rmii (0x11) | Rx data valid  |
| ETH_RX_ER    | P7_07  | k_ra_psel_ether_rmii (0x11) | Rx error       |
| ETH_CRS_DV   | P7_08  | k_ra_psel_ether_rmii (0x11) | Carrier sense  |

## How to test

1. `make build` from this directory, `make flash` to load `.hex`.
2. Plug an Ethernet cable from the EK-RA8D2's J64 receptacle into a
   peer host (laptop, switch). The on-board PHY auto-negotiates.
3. On the host, set a static address in the same subnet, e.g.:
   - macOS:    `sudo ifconfig en0 192.168.1.10/24`
   - Linux:    `sudo ip addr add 192.168.1.10/24 dev eth0`
4. `ping 192.168.1.42` -- should answer at ~1 ms RTT, LED1 toggling
   on each ICMP echo-request and LED2 on each reply.
5. Open the J-Link OB CDC port at 115200 8N1 (e.g.
   `picocom -b 115200 /dev/cu.usbmodem...`) for the SCI8 log stream.
6. `nc 192.168.1.42 7` (or `telnet 192.168.1.42 7`) -- the firmware
   accepts the SYN, completes the handshake, and echoes any line
   typed on the host. Close the connection (`Ctrl-D` / `Ctrl-]`)
   and the FW completes the FIN exchange before returning to LISTEN.

## Limitations

- **Single TCP connection**. A second client SYN is rejected (the
  firmware is busy in CLOSE-WAIT / LAST-ACK / ESTABLISHED).
- **No retransmit**. If a host drops our ACK / SYN-ACK the connection
  hangs. This is a smoke test for the descriptor ring, not a stack.
- **No DHCP**. Static address only, hard-coded.
- **No fragmentation**. Frames over 1514 B are dropped.

## Status

Hardware bring-up. Validates the sweep-2 descriptor-ring drivers end
to end. CI builds for the cross-target only; there is no host-side
unit test (the byte-level builders are exercised through other apps'
test suites).
