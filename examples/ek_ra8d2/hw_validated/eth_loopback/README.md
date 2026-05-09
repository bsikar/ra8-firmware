# eth_loopback

ETHA per-port internal loopback bring-up demo for the bare EK-RA8D2 EVM
(no off-chip PHY traffic required).

Brings ETHA port 0 through `init -> set_mode CONFIG ->
descriptor_ring_init -> set_mode OPERATION -> account_traffic ->
get_stats -> deinit`, then logs `etha: loopback ok` over the J-Link OB
CDC console (SCI8 @ 115200 8N1).

Build / flash:

```
make eth_loopback
make -C examples/ek_ra8d2/eth_loopback flash
```
