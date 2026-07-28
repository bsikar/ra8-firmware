# threadx_netx_tcp_echo -- NetX Duo TCP echo (hw_validated/hil)

The canonical networking demo: ThreadX + NetX Duo serve an RFC 862
TCP echo on `192.168.1.42:7` over the EK-RA8D2's R-Switch ethernet
path (ETHA + RMAC + GWCA descriptor DMA). Each served echo is logged
to the SCI8 console as `[netx] echoed N bytes from a.b.c.d`.

## How it is gated

- **HIL (real hardware, Pi as the peer)**: `HIL_MODE=hil_eth_tcp` in
  `hil.conf` drives `scripts/hil/eth_tcp.sh` -- the Pi assigns itself
  `192.168.1.1` on the board-facing wired interface (now its built-in
  `eth0`, not the USB adapter), opens TCP to the board, sends
  `HIL_PAYLOAD_BYTES` random bytes, and asserts a byte-exact echo.
  Bench-verified byte-exact at 8, 64, 256, 512 and 1024-byte payloads;
  the driver's MTU=128 clamp plus IP fragmentation keeps every single
  frame clear of the accepted large-frame TX silicon limitation (#21).

  This is the **only** app in the tree with `HIL_MODE=hil_eth_tcp`, so it
  is the only gate that puts anything on a wire. That matters: while it
  sat in `hil_needs_revalidation/` under a blocker that turned out to be
  wrong, a TX data-corruption regression lived on `dev` for a month with
  nothing able to see it (#499). Do not demote this app without first
  establishing that the failure really is the bench.

- **EIL (ra8_emulator, no hardware)**: `scripts/emu/eil_all.sh` boots the
  same `.elf` headless. ra8_emulator ships the peer in-process --
  `tools/ra8_emulator` models the R-Switch register cluster
  (`board_periph_eth.c`) and its virtual host `board_net`
  (192.168.1.1) resolves the firmware over ARP, pings it, TCP-connects
  to port 7, sends a fixed 23-byte payload, and byte-verifies the
  echo. The EIL verdict asserts both the firmware's served-echo UART
  banner (`HIL_EXPECT`) and the peer's end-of-run `echo MATCH` report.

## Run it locally in ra8_emulator

```sh
make threadx_netx_tcp_echo
tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/threadx_netx_tcp_echo/build/threadx_netx_tcp_echo.elf
```

The UART shows the boot banners, `eth: ready`, then
`[netx] echoed 23 bytes from 192.168.1.1`; the end-of-run report's
`NET TCP` line gives the peer's byte-exact verdict.
