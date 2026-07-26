# threadx_netx_tcp_echo -- NetX Duo TCP echo (hw_validated/hil)

The canonical networking demo: ThreadX + NetX Duo serve an RFC 862
TCP echo on `192.168.1.42:7` over the EK-RA8D2's R-Switch ethernet
path (ETHA + RMAC + GWCA descriptor DMA). Each served echo is logged
to the SCI8 console as `[netx] echoed N bytes from a.b.c.d`.

## How it is gated

- **HIL (real hardware, Pi as the peer)**: `HIL_MODE=hil_eth_tcp` in
  `hil.conf` drives `scripts/hil/eth_tcp.sh` -- the Pi assigns itself
  `192.168.1.1` on its USB-Ethernet adapter, opens TCP to the board,
  sends `HIL_PAYLOAD_BYTES` random bytes, and asserts a byte-exact
  echo. The 256-byte payload sits inside the bench-proven
  corruption-free window (8..512-byte payloads round-trip byte-exact;
  600+ bytes hits the accepted large-frame TX silicon limitation, so
  the driver keeps its MTU=128 clamp).

- **SIL (board_sim, no hardware)**: `scripts/sim/sil_all.sh` boots the
  same `.elf` headless. board_sim ships the peer in-process --
  `tools/ra8_emulator` models the R-Switch register cluster
  (`board_periph_eth.c`) and its virtual host `board_net`
  (192.168.1.1) resolves the firmware over ARP, pings it, TCP-connects
  to port 7, sends a fixed 21-byte payload, and byte-verifies the
  echo. The SIL verdict asserts both the firmware's served-echo UART
  banner (`HIL_EXPECT`) and the peer's end-of-run `echo MATCH` report.

## Run it locally in board_sim

```sh
make threadx_netx_tcp_echo
tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/threadx_netx_tcp_echo/build/threadx_netx_tcp_echo.elf
```

The UART shows the boot banners, `eth: ready`, then
`[netx] echoed 21 bytes from 192.168.1.1`; the end-of-run report's
`NET TCP` line gives the peer's byte-exact verdict.
