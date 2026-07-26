# rtt_log_demo

Minimal SEGGER RTT logging demo for the EK-RA8D2. Allocates a
SEGGER-compatible RTT control block in SRAM and writes
`rtt_log_demo: NNN` lines via the in-tree `rtt_write` shim. LED1
toggles every second alongside each line. Host-side a SEGGER
`JLinkRTTViewer` (or `JLinkRTTLogger`) attached over SWD captures
the output -- the J-Link OB UART is NOT used.

Build / flash:

```
make rtt_log_demo
make -C examples/ek_ra8d2/rtt_log_demo flash
```

## HIL plan

**HIL-able after harness work -- needs a new `hil_rtt_scrape` mode.**
The firmware emits its banner over SEGGER RTT, not UART, so the
existing `uart_scrape` helper sees nothing. RTT is exposed by the
J-Link API (`JLinkRTTClient`, `JLink_RTTERMINAL_*` in the SDK), so a
new scraper script -- something like `hil_rtt_scrape.sh` modelled on
`hil_run_direct.sh` -- could attach to the same J-Link the rest of
the suite already drives and assert the `rtt_log_demo:` prefix
appears within `HIL_TIMEOUT_S`.

Proposed `hil.conf`:

```
HIL_MODE=rtt_scrape
HIL_EXPECT="rtt_log_demo: "
HIL_TIMEOUT_S=10
```

Until the new helper exists this is the closest fallback:
`hil_check_alive` proves the firmware is alive (PC in MRAM,
CycleCnt advancing, LED1 toggle visible in g_blink_tick-style
probe), but does NOT prove the RTT control block was actually
populated.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not
yet bench-confirmed the RTT output path.

## board_sim / SIL

board_sim models the RTT host side (`board_periph_rtt.c`): it scans
the emulated SRAM for the `"SEGGER RTT"` control-block ID exactly as
the J-Link OB does, drains up-buffer 0 (respecting the ring wrap and
storing the advanced read offset back), and echoes each completed
line to the console as `[rtt] ...`. `scripts/sim/sil_all.sh` therefore
checks this app's `hil.conf` banner headlessly, just like a
`uart_scrape` app:

```
BOARD_SIM_STOP_ON="rtt_log_demo: 3" \
  tools/ra8_emulator/build/ra8_emulator build/rtt_log_demo.elf
```
