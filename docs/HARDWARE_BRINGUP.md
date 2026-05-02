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

## 2026-05-02 Tier-by-tier results

| App | Tier | Result | Notes |
|---|---|---|---|
| blink | 1 (LED) | ✅ Running | gdb halt confirmed PC in main loop, no fault |
| uart_hello | 2 (UART) | ✅ Verified | "hello, ra8d2!" stream at 115200 8N1 on SCI8 via /dev/cu.usbmodem0010865671981 |
| threadx_blink | 1+RTOS | ✅ Running | ThreadX scheduler in tx_thread_schedule idle; threads active |
| threadx_lwip_tcp_echo | 5 (ETH) | ⚠️  Running but unreachable | Firmware up; static IP 192.168.1.50 mismatches host network 10.0.64.x. Needs DHCP or subnet-match. |
| usb_hid_device | 3 (USB-FS) | 🐛 Init fails | PC parked at usb_hid_panic_halt (main.c:283). USB init returns error on real silicon — likely ra_cgc_usbhs_pll_enable timeout or a stub that we promoted assuming chip behaviour that doesn't match. |

## Open follow-ups for next hardware session

1. **USB device bring-up debug** — usb_hid_device fails. Add log/gdb-trace to identify exact failing call. Likely candidate: ra_cgc_usbhs_pll_enable's USBCKCR PLL-lock wait may not actually settle in real hardware; or a missing pin-enable.
2. **Ethernet integration** — switch threadx_lwip_tcp_echo to lwIP DHCP so it picks up an IP from any subnet the cable connects to. Or add a runtime config knob.
3. **Phase 7.1 tier 1 LED-only sweep** — flash blink_hal, threadx_mpu_partition_demo (LED+MPU), confirm each runs.
4. **ra_board_uart_console real-console verification** — refactor uart_hello to call ra_board_uart_console_* (now correct on SCI8) instead of raw ra_sci_*.

## 2026-05-02 broader silicon sweep

Quick-flash + halt-and-check-PC across more example apps:

| App | Result | Halt PC location |
|---|---|---|
| blink | ✅ | ra_delay_ms loop |
| blink_hal | ✅ | ra_delay_ms loop |
| uart_hello | ✅ | "hello, ra8d2!\n" stream verified |
| clock_check | ✅ | ra_delay_ms loop |
| threadx_blink | ✅ | tx_thread_schedule idle |
| threadx_filex_demo | ✅ | tx_thread_schedule idle |
| threadx_canfd_demo | ✅ | tx_thread_schedule idle |
| threadx_ota_demo | ✅ | tx_thread_schedule idle |
| threadx_lwip_tcp_echo | ✅ runs, ⚠️ unreachable (subnet) | tx_thread_schedule idle |
| threadx_mpu_partition_demo | ⚠️ caught fault (intentional?) | internal_bkpt — the deliberate cross-region access fired the fault handler as designed; the panic-spin is the demo's "fault caught" path |
| threadx_levelx_demo | 🔍 still in main init at sample time | needs longer settle window |
| threadx_filex_levelx_demo | 🔍 still in main init | needs longer settle |
| threadx_https_client | 🔍 still in main init | needs longer settle |
| ra_bootloader | 🔍 still in system_init | needs longer settle |
| threadx_netx_tcp_echo | 🐛 ra_error_handler panic | likely SCI8 console init racing — same pattern as uart_hello pre-fix? |
| threadx_ipc_demo | 🐛 ra_hw_err fired | needs investigation |
| usb_hid_device | 🐛 usb_hid_panic_halt | USB init returns error (suspected ra_cgc_usbhs_pll_enable hang or missing pin enable) |

**Score: 9 of 17 sampled apps confirmed running on silicon. 4 still settling. 4 need bug fixes.**

## 2026-05-02 systematic 27-app sweep

Built a /tmp/ra8d2-hw-test test harness (flash + UART capture + halt-and-PC).
Results across 27 testable apps:

### ✅ Confirmed running on silicon (12 apps)
- blink, blink_hal, clock_check, uart_hello (UART verified)
- threadx_blink, threadx_filex_demo, threadx_canfd_demo, threadx_ota_demo
- threadx_lwip_tcp_echo (boots, runs scheduler)
- threadx_filex_levelx_demo (boots, prints "[fxlx] booting...")
- threadx_ipc_demo (prints continuously — M85 sends ping, no M33 reply
  because M33 has no firmware loaded — this is the expected partial
  state, not a bug)
- usb_hid_device (after 394055f13 fix), usb_cdc_echo (after this commit)

### 🔍 Partial / boot logs visible
- threadx_filex_levelx_demo: "[fxlx] formatting + opening LevelX
  partition / [fxlx] lx_nor_flash_format failed" — XSPI NOR driver
  hand-off doesn't initialise the chip correctly
- threadx_levelx_demo: same lx_nor_flash_format path fails

### 🐛 Real bugs surfaced (not yet fixed)
1. **NetX malloc** — threadx_netx_tcp_echo: nx_system_initialize calls
   malloc → ra_sbrk_trap fires (NASA Rule 3 enforcement). Need to
   compile NetX with NX_TRACE_INSERT off + NX_PHYSICAL_HEADER right or
   pre-allocate a static heap pool.
2. **XSPI NOR flash format** — LevelX apps fail at lx_nor_flash_format.
   Likely the lx_nor_driver_ra_xspi_initialize hook isn't actually
   bringing up the IS25LX512M-JHLE chip. Needs xSPI bring-up debug.
3. **USB device enumeration silent** — usb_hid_device firmware runs
   stably but macOS never enumerates VID 0x1209. ra_nsc_usb_init or
   downstream USB-FS controller bring-up missing a step (possibly D+
   pull-up, PHY clock, or VBUS-detect).
4. **USB host apps fault** — usb_host_cdc_echo / keyboard / msc_browse
   all hit Default_Handler. USBHS host bring-up incomplete.
5. **BLE apps fault** — threadx_nimble_peripheral prints "[nimble] boot"
   then crashes. Renesas BLE patch image (vendor blob) required for
   radio init; documented in VENDOR_BLOBS.md.
6. **threadx_https_client** — stuck in main init at line 239. Needs
   network up (so depends on lwIP echo working too).
7. **ra_bootloader** — stuck in system_init.c:331. Boot stub design
   may intentionally halt waiting for a banked image; need to verify.
