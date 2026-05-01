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
