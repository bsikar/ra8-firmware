# crc_demo

CRC-32 hardware-vs-software cross-check for the bare EK-RA8D2 EVM.

Brings up SCI8 (115200 8N1) and the on-chip CRC unit. Once a second
computes the CRC over a fixed 16-byte test buffer through both the
hardware engine (`ra8_crc_compute`, polynomial `k_ra8_crc_poly_32_ieee802_3`)
and a tiny in-tree bit-serial reference implementation, then prints
`crc: hw=XXXXXXXX sw=XXXXXXXX match=Y` on the J-Link OB CDC channel.

- LED1 toggles on every successful match.
- LED2 toggles on a mismatch (should never happen).

No external hardware is required.

Build / flash:

```
make crc_demo
make -C examples/ek_ra8d2/crc_demo flash
```
