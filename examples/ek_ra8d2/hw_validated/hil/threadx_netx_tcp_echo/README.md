# Pi-as-peer ethernet app (hw_pending)

threadx_netx_tcp_echo is the canonical networking demo: it runs on
the EK-RA8D2's GMAC (ETHA1 + RMAC1 + GWCA0) and expects a Pi peer to
ARP / ICMP / TCP into it. NetX Duo provides the IPv4 stack.

## Why these are `hw_pending`

JTAG probing of the live HIL board on 2026-05-19 confirmed:

- ETHA1 reaches EAMC=EAMS=OPERATION correctly.
- RMAC1 is programmed by ra8_board_ethernet_init.
- GWCA0_CTRL has EDTR+EDRR set (TX/RX engines enabled).
- The PHY reports link up on the wire side.

BUT every app reaches "eth: ready", then `ra8_eth_read` returns
`k_ra8_err_no_data` forever and no ARP/ICMP reply ever leaves the
chip. Root cause: the HAL's GWCA scaffold
(`libs/ra8_hal/inc/ra8_ether_regs.h::r_gwca_regs_t`) is a STUB --
it covers GWCA_CTRL/STS/IE/ICLR for lifecycle + IRQ, but not the
**descriptor list address registers** (GWDCC / GWTRC / GWRDB /
equivalent) that point the chip at the SW-side
`s_rx_descriptors[]` and `s_tx_descriptors[]` arrays. Without
those, the chip's RX engine has no idea where to write inbound
frames, and the TX engine has no descriptor ring to walk.

In other words: `ra8_eth_open` enables the engines but never tells
the engines about the buffers. The descriptors are correctly laid
out in SW (RACT bits set, p_next ring, etc.) but the chip has no
pointer to them.

## How to graduate back to `hw_validated/hil/`

1. Port the full GWCA register set from HUM Ch 35 + 36 (RA8D2 HUM
   pages 1800..) into `ra8_ether_regs.h::r_gwca_regs_t`.
2. Add `ra8_gwca_bind_descriptors(tx_ring, rx_ring, depth, payload)`
   in `libs/ra8_hal/src/ra8_eth.c` that writes the descriptor base
   addresses + per-direction config registers.
3. Call the bind helper from `ra8_eth_open` between
   `internal_init_rings` and the EDTR/EDRR kick.
4. Verify with `eth_loopback` that the descriptor ring still
   walks (the existing MAC-only test is the smoke check), then
   re-promote each of the 5 apps after the Pi-as-peer probe lands.

`eth_loopback` continues to pass because it exercises the MAC-only
loopback path that doesn't depend on the descriptor list.
