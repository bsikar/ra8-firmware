# Hardware Bring-up Report (EK-RA8D2 v1)

**Probe**: J-Link OB-RA4M2, SN 1086567198, accessed via JLinkExe v9.38a
**Tool chain**: arm-none-eabi-gcc, JLinkExe (SEGGER)
**Date**: 2026-05-02

## Apps successfully flashed and verified running

### blink (commit f55b0... post-fix)
- `make blink` builds clean
- `JLinkExe loadfile blink.hex` succeeds (143 KB/s, 6144 bytes to MRAM)
- After flash + run + 3s sleep + halt: **PC=0x02000B2E in `ra_delay_ms` at libs/ra_core/src/ra_time.c:94, called from main.c:67**
- LR=0x02000245 (return to main loop `b.n 02000230`)
- CycleCnt advancing (~25M cycles in 3s = 8.3 MHz effective — chip running but maybe not yet at full 1 GHz)
- **No fault, no Default_Handler trap.** Firmware in main loop.

### uart_hello
- Flashes and runs without fault (PC in `ra_delay_ms` from main.c:192).
- **BUT no UART data reaching the host CDC port**. SCI3 registers populated; suspected baud-rate drift (BRR=0x20 for 115200 at PCLKA=125MHz gives 118371 actual = 2.7% error, edge of UART tolerance).

## Real bugs caught (silent-failure on hardware)

1. **SysTick_Handler weak-alias bug** — both `vector_table.c` and `ra_time.c` defined SysTick weak; ld picked the alias-to-Default_Handler in vector_table.c, silently discarding ra_time.c's real implementation. Without the fix `ra_delay_ms` would spin forever. **Fixed in commit d3a9a278f** across all 36 example apps.

2. **uart_hello SCI channel was wrong** — used SCI8, J-Link OB VCOM is on SCI3 per the BSP commit a937aecbf and UM Table 13. Fixed.

## Suspected bugs needing follow-up

1. **SCI BRR computation** — baud error at the UART tolerance limit. Either:
   - SCICLK isn't actually running at 100 MHz (would explain why N=33 instead of 27)
   - PCLKA reported by `ra_cgc_get_clock_hz` is 125 MHz but actual SCICLK divider isn't /4
   - MDDR fine-tuning needs to be applied
   
   Need to verify by reading CGC registers (PLLCCR, MOSCCR, etc.) on hardware or use a logic analyzer to measure actual baud.

2. **J-Link OB VCOM bridge** — even with correct SCI config the J-Link OB CDC may need explicit VCOM enable via JLinkConfig. Or the on-board J-Link OB might bridge a different SCI channel than the user-facing SCI3.

## Test logs
- /tmp/ra8d2-hw-test/02_flash_blink.log — first successful flash
- /tmp/ra8d2-hw-test/05_uart_capture.txt onward — empty UART captures
- All zero-byte UART captures despite firmware actively running in main loop

## 2026-05-02 follow-up: UART working

Caught additional bugs via continued hardware bring-up:

3. **BSP UART console SCI channel was wrong** — `k_ra_board_uart_console_sci_channel`
   was set to `3U` by the original BSP-additions agent (commit a937aecbf).
   Sweeping channels 0..9 on real silicon revealed the J-Link OB VCOM
   bridge is on **SCI8**, not SCI3. PD02/PD03 routing under PSEL=`sci_async`
   maps to SCI8 on EK-RA8D2 v1. **Fixed.**

4. **Wrong serial port** — `/dev/cu.usbmodem508RMDZL10983` is something
   else (possibly a parallel DAPLink interface). The actual J-Link OB VCOM
   bridge is **`/dev/cu.usbmodem0010865671981`** (matches J-Link SN
   001086567198).

### Verified output
At 115200 8N1 (with 2.7% baud-rate drift accepted by the J-Link OB CDC bridge):
```
hello, ra8d2!
hello, ra8d2!
hello, ra8d2!
...
```
SCI8 -> J-Link OB UART bridge -> USB-CDC -> /dev/cu.usbmodem0010865671981 -> host.
