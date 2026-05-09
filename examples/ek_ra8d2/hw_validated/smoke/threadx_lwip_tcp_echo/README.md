# threadx_lwip_tcp_echo

ThreadX + lwIP TCP echo demo for the EK-RA8D2.

## What it does

Brings up SCI8 (115200 8N1, J-Link OB CDC), routes the eleven RMII pins on
J64 to the on-chip Ethernet MAC, then enters ThreadX. lwIP runs on top of
the ThreadX-backed `sys_arch` port (`port/lwip/arch/sys_arch.c`) and the
hardware Ethernet netif glue (`port/lwip/netif/ra_etha_netif.c`), and
listens on **TCP port 7** at the static IPv4 address
**192.168.1.50 / 24**.

Every received segment is echoed back unchanged via lwIP's BSD-style
sockets API. This is the lwIP counterpart to `examples/threadx_netx_tcp_echo`
(which runs NetX Duo against the same hardware path).

## Test recipe

1. Wire the EK-RA8D2 J64 RMII port to a host PC's Ethernet jack (direct
   crossover or through a switch).
2. Configure the host's NIC for `192.168.1.x / 24` (any address other
   than `.50`).
3. Flash and reset:
   ```
   make flash
   ```
4. Open the J-Link OB CDC port at 115200 8N1 to see the boot banner.
5. From the host:
   ```
   nc 192.168.1.50 7
   ```
   Anything you type comes back. The board's UART log will show
   `[lwip] echoed N bytes` for each receive.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED init/toggle (per UM Table 24 p
31) and `ra_board_ethernet_init` for the on-board PEF7071 PHY. The
v1 board's PHY is wired RGMII on P304..P307 / P906..P909 / P206 /
P905 per UM Table 26 p 33; the BSP veneer matches that pin set.

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Section 6.1 + Tables 13 p 24 / 24 p 31 / 26 p 33, and HUM
(R01UH1065EJ0130) Ethernet chapter.
