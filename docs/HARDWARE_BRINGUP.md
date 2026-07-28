# Hardware Bring-up Report (EK-RA8D2 v1)

**Probe**: J-Link OB-RA4M2 (serial in `.env` `JLINK_SN`), accessed via JLinkExe v9.38a
**Tool chain**: arm-none-eabi-gcc, JLinkExe (SEGGER)
**Date**: 2026-05-02

## 2026-06-10 OSPI flash RESOLVED -- JEDEC 0x9D reads, round-trip passes (#44)

**The flash was never a hardware fault. It was a controller chip-select
bug in our `ra8_xspi` driver.** Every prior "hardware" conclusion in this
file (SW4-3 isolation, missing 1.8 V rail, U15 gating) is **superseded** by
this entry. Proof the bug was firmware: J-Link's own OSPI flash loader
erases/programs/verifies arbitrary patterns at the `0x90000000` memory
bank at ~100 KB/s -- the part is present, powered, and on the bus at all
times. Our driver simply talked to the wrong chip-select.

### Root cause

The on-board IS25LX512M's chip-select net (`OSPI_FLASH_S_L`, P104; EK-RA8D2
UM Table 29 p 35) is wired to the xSPI controller's **CS1** line, not CS0.
The Renesas FSP EK-RA8D2 OSPI example confirms this: its
`configuration.xml` sets `module.driver.ospi_b.channel = channel.1`, and
FSP `R_OSPI_B_Open` writes the protocol/timing into
`LIOCFGCS[p_cfg_extend->channel]` and selects `CDCTL0.CSSEL = channel` on
every manual command. Our `ra8_xspi_init` hard-coded CS0: it wrote the
protocol into `LIOCFGCS[0]` and left `CDCTL0.CSSEL = 0`. With CSSEL=0 the
controller strobed the (unconnected) CS0 pin, the flash never saw
chip-select, and the 1S RDID floated to `0x00FFFFFF` on the board
pull-ups -- the exact "bus silence" signature mis-attributed to hardware.

A secondary, also-vendor-mandated step: after the controller protocol
config is in place, the FSP bring-up (`ospi_b_ep.c` `ospi_b_init`,
`ospi_flash_issi_is25lx512.c` `reset_ospi_device`) pulses
`LIOCTL.RSTCS` (bit 16) low->high to hardware-reset the flash. A prior
J-Link OSPI loader run can leave the part in OPI/DOPI; without the RSTCS
pulse it would not answer a 1S RDID. Both fixes are now in
`ra8_xspi_init`.

### The fix (minimal, in `ra8_xspi.c` + `ra8_ospi_regs.h`)

- `ra8_xspi_init` now writes the protocol mode to `LIOCFGCS[k_ra8_xspi_onboard_cs]`
  (CS1) and sets `CDCTL0.CSSEL = k_ra8_xspi_onboard_cs`, so all subsequent
  manual commands (RDID, RDSR, WREN, PP, SE, read) target the connected
  device. `k_ra8_xspi_onboard_cs = 1` is a documented board fact.
- `ra8_xspi_init` then calls `internal_xspi_reset_device()`, which pulses
  `LIOCTL.RSTCS` low->high (with `WPCS` held deasserted) to return the
  flash to its power-on 1S SPI protocol.
- `ra8_ospi_regs.h` gained `k_ra8_xspi_lioctl_mask_wpcs` (bit 0) /
  `k_ra8_xspi_lioctl_mask_rstcs` (bit 16) from the FSP CMSIS
  `R_XSPI0_LIOCTL_*` field definitions.

### On-board verification (flash_journal, J-Link memprobe)

| Symbol | Address | Value |
| --- | --- | --- |
| `g_fj_jedec_id` | 0x22000060 | **`0x009D5A1A`** -- mfr **0x9D** (ISSI IS25LX512M) |
| `g_fj_match` | 0x2200004C | advancing (3 -> 14 -> ... over the run) |
| `g_fj_mismatch` | 0x22000050 | **0** (zero failures) |
| `g_fj_last_step` | 0x22000054 | **4** = `k_fj_step_compare_ok` |

Full erase -> program -> read-back -> compare round-trips pass with zero
mismatches: the JEDEC ID reads 0x9D **and** real data round-trips through
the part. Issue #44 is resolved in firmware; no hardware rework needed.

---

## 2026-06-10 OSPI flash: exhaustive software-side sweep (issue #44)

Built on the prior conclusion (controller + pins exonerated) by ruling out
every remaining *software-reachable* cause for the silent on-board
IS25LX512M Octo-SPI flash. A throwaway `flash_journal/main.c` probe drove
each experiment and stamped J-Link-readable globals; `main.c` was reverted
to pristine afterward. All reads were taken over SWD on the live board.

### What was tried (all firmware-only, no physical changes)

1. **U15 expander 256-value output sweep.** For every output byte
   `0x00..0xFF` (IODIR=0xFF all-outputs, HiZ=0x00), pulse RESET_L (P106),
   re-init OSPI, read 1S RDID. **Result: zero configs produced a real ID**
   (`g_sweep_n_nonfloat = 0`). U15 GPIO outputs are NOT in the OSPI bus
   path.
2. **U15 released / Hi-Z / per-bit overrides.** IODIR=0x00 (release the
   override so physical SW4 governs the analog mux unopposed), all-Hi-Z,
   and each of the 8 lines driven individually 0 then 1. **Result: still
   no real ID** in any case. With U15 released to inputs, its input
   register reads **0xF8** -- the same value the previously-rejected decode
   saw; the exact U15<->SW4 bit mapping is not published in the UM (it is in
   the EK-RA8D2 Design Package schematic) and has not been verified on this
   EVM, so this value is NOT actionable and was not used to recommend any
   switch position.
3. **Software reset (RSTEN 0x66 + RST 0x99) in BOTH 8D and 1S**, to recover
   a chip possibly stuck in OPI from a prior boot. After each, re-init 1S
   and RDID. **Result: no recovery** (RDID stays 0x00000000 / 0x00FFFFFF).
4. **WREN -> RDSR WEL-toggle test (1S).** Triggered WREN via a sector
   erase, then read RDSR before/after. **Result: WEL never sets** (RDSR
   reads 0x00, WEL bit-1 stays 0) -- the flash does not answer WREN.
5. **GPIO continuity probe of the OCTA lines.** With OSPI de-inited, DQ0
   (P100), DQ1 (P803, the 1S CIPO), and CS (P104) read **high with both
   pull-up and no-pull** -- i.e. the board's external bus pull-ups hold
   the lines high; nothing on the bus is being actively driven low.
6. **Multi-mode RDID** (1S and 8D): 1S floats `0x00FFFFFF`; 8D times out on
   DQS.

### Live register evidence (re-confirmed this session)

```
PFS P104 (CS)    = 0x1C010002   <- PSEL=0x1C (OSPI), PMR=1, routed
PFS P100 (DQ0)   = 0x1C010002   <- OSPI routed
PFS P808 (CK)    = 0x1C010000   <- OSPI routed
PFS P106 (RST_L) = 0x00000007   <- GPIO out, driven HIGH (released)
PFS P105 (ERR_L) = 0x00000002   <- GPIO in, reads HIGH (no flash error)
XSPI0 COMSTT     = 0x00770000    XSPI0 INTS = 0x00000000 (CMDCMP cleared)
U15 devid(0x01)  = 0xA0   IODIR/OUTPUT/HIZ read back exactly as written
U15 input(0x0F)  = 0xF8 (released)   expander ACKs every txn
```

### Conclusion

Every software-reachable lever has now been exercised: pin routing, the
OSPI controller/command engine (CMDCMP completes; CDT opcode
left-justification fix retained), OCTACLK bring-up, RESET_L timing,
software-reset in both protocol modes, the WREN/WEL path, and the **entire
U15 expander configuration space** (all 256 outputs, released-inputs,
Hi-Z, per-bit). None connects the flash. The U15 expander is proven to be
a pure SW4 sense/override that does NOT gate the OSPI bus -- so it cannot
be the fix, and the firmware override of it is irrelevant to reachability.

The flash is electrically silent and the OCTA data lines sit at the board
pull-up level. The only mechanism that physically connects the IS25LX512M
DQ/CK/CS to the MCU is the **SW4-3 analog mux**, which is hardware-only and
not reachable from firmware (the U15 override cannot move it). With the
controller and firmware fully exonerated by the evidence above, **the one
remaining check is physical: confirm on the board / with a meter or 'scope
that the SW4-3 analog switch is actually passing the OSPI DQ/CK/CS lines
through to U3** (continuity from each MCU OCTA pad to the corresponding
IS25LX512M pin while a manual command clocks). No further firmware change
is justified.

> Note on part ID: the on-board part is the **ISSI IS25LX512M (mfr 0x9D)**,
> not Macronix (0xC2). A correct 1S RDID on a connected part returns 0x9D.



Hand-flashing 26 apps and eyeballing the halt-PC (as the early-May tables
below were generated) does not scale. The authoritative HIL sweep lives
on the Pi 5 self-hosted runner (`.github/workflows/hil.yml`) and is
driven by `scripts/hil/all.sh`, which auto-discovers every app under
`examples/ek_ra8d2/hw_validated/hil/` and verifies each one against its
`hil.conf` manifest. The mode-specific helpers (`hil_run_direct.sh` for
UART scrape, `hil_usb_test.sh` for USB CDC echo, `hil_jlink_memprobe.sh`
for live counter probe, `hil_eth_tcp.sh` for ethernet socket echo,
`hil_check_alive.sh` for the fault-recovery demo) are documented in
[`HIL_SUITE.md`](HIL_SUITE.md) and [`HIL_DEVELOPER_WORKFLOW.md`](HIL_DEVELOPER_WORKFLOW.md).

The historical halt-PC sweeps recorded below were captured by hand
against a developer laptop wired to the EK-RA8D2; they remain as a
bring-up log, not as a description of the current CI gate.

## examples/ tier layout

Applications under `examples/` are organised by hardware-support tier
so a developer can see at a glance which apps the project can
hardware-validate on the stock EVM:

| Path                            | Meaning                                                                 |
|---------------------------------|-------------------------------------------------------------------------|
| `examples/ek_ra8d2/<app>/`      | Validates on a stock EK-RA8D2 v1 with no extra peripherals (or only a $5 USB device for the host-port demos). The pre-commit hook and CI smoke-test apps from this tier. |
| `examples/_unsupported/<app>/`  | Requires extra hardware we do not have (audio amp, BLE radio + vendor patch image, RSIP BIST blob, PTP switch, MCK motor board, SD card slot). Cross-builds in CI but is not flashed; expect bit-rot until somebody acquires the hardware. |

The build-target name is just the bare app directory name; the tier
directory is purely organisational. `make blink` builds
`examples/ek_ra8d2/blink/build/blink.elf`; `make motor_3phase` builds
`examples/_unsupported/motor_3phase/build/motor_3phase.elf`. The
top-level `Makefile` and `CMakeLists.txt` auto-discover apps under
`examples/<tier>/<app>/`.

When adding a new app, drop it under whichever tier matches the
hardware story. See [`examples/ek_ra8d2/README.md`](../examples/ek_ra8d2/README.md)
and [`examples/_unsupported/README.md`](../examples/_unsupported/README.md).

## Apps successfully flashed and verified running

### blink (commit f55b0... post-fix)
- `make blink` builds clean
- `JLinkExe loadfile blink.hex` succeeds (143 KB/s, 6144 bytes to MRAM)
- After flash + run + 3s sleep + halt: **PC=0x02000B2E in `ra8_delay_ms` at libs/ra8_core/src/ra8_time.c, called from main.c**
- LR=0x02000245 (return to main loop `b.n 02000230`)
- CycleCnt advancing (~25M cycles in 3s = 8.3 MHz effective -- chip running but maybe not yet at full 1 GHz)
- **No fault, no Default_Handler trap.** Firmware in main loop.

### uart_hello
- Flashes and runs without fault (PC in `ra8_delay_ms` from main.c).
- **BUT no UART data reaching the host CDC port**. SCI3 registers populated; suspected baud-rate drift (BRR=0x20 for 115200 at PCLKA=125MHz gives 118371 actual = 2.7% error, edge of UART tolerance).

## Real bugs caught (silent-failure on hardware)

1. **SysTick_Handler weak-alias bug** -- both `vector_table.c` and `ra8_time.c` defined SysTick weak; ld picked the alias-to-Default_Handler in vector_table.c, silently discarding ra8_time.c's real implementation. Without the fix `ra8_delay_ms` would spin forever. **Fixed in commit d3a9a278f** across all 36 example apps.

2. **uart_hello SCI channel was wrong** -- used SCI8, J-Link OB VCOM is on SCI3 per the BSP commit a937aecbf and UM Table 13. Fixed.

## Suspected bugs needing follow-up

1. **SCI BRR computation** -- baud error at the UART tolerance limit. Either:
   - SCICLK isn't actually running at 100 MHz (would explain why N=33 instead of 27)
   - PCLKA reported by `ra8_cgc_get_clock_hz` is 125 MHz but actual SCICLK divider isn't /4
   - MDDR fine-tuning needs to be applied
   
   Need to verify by reading CGC registers (PLLCCR, MOSCCR, etc.) on hardware or use a logic analyzer to measure actual baud.

2. **J-Link OB VCOM bridge** -- even with correct SCI config the J-Link OB CDC may need explicit VCOM enable via JLinkConfig. Or the on-board J-Link OB might bridge a different SCI channel than the user-facing SCI3.

## Test logs
- /tmp/ra8d2-hw-test/02_flash_blink.log -- first successful flash
- /tmp/ra8d2-hw-test/05_uart_capture.txt onward -- empty UART captures
- All zero-byte UART captures despite firmware actively running in main loop

## 2026-05-02 follow-up: UART working

Caught additional bugs via continued hardware bring-up:

3. **BSP UART console SCI channel was wrong** -- `k_ra8_board_uart_console_sci_channel`
   was set to `3U` by the original BSP-additions agent (commit a937aecbf).
   Sweeping channels 0..9 on real silicon revealed the J-Link OB VCOM
   bridge is on **SCI8**, not SCI3. PD02/PD03 routing under PSEL=`sci_async`
   maps to SCI8 on EK-RA8D2 v1. **Fixed.**

4. **Wrong serial port** -- the other `/dev/cu.usbmodem*` node on the bus is
   something else (possibly a parallel DAPLink interface). The actual J-Link OB
   VCOM bridge is the node whose digits match the probe's own serial,
   **`/dev/cu.usbmodem<JLINK_SN>1`**. Find it with `make hil-find-jlink` and
   pin it through `JLINK_SN` / `RA8_CONSOLE_TTY` in `.env` (template:
   `.env.example`) -- bench-specific serials are deliberately not recorded in
   the tree.

### Verified output
At 115200 8N1 (with 2.7% baud-rate drift accepted by the J-Link OB CDC bridge):
```
hello, ra8d2!
hello, ra8d2!
hello, ra8d2!
...
```
SCI8 -> J-Link OB UART bridge -> USB-CDC -> /dev/cu.usbmodem<JLINK_SN>1 -> host.

## 2026-05-02 Tier-by-tier results

| App | Tier | Result | Notes |
|---|---|---|---|
| blink | 1 (LED) | [PASS] Running | gdb halt confirmed PC in main loop, no fault |
| uart_hello | 2 (UART) | [PASS] Verified | "hello, ra8d2!" stream at 115200 8N1 on SCI8 via the J-Link OB VCOM node |
| threadx_blink | 1+RTOS | [PASS] Running | ThreadX scheduler in tx_thread_schedule idle; threads active |
| threadx_lwip_tcp_echo | 5 (ETH) | [WARN]  Running but unreachable | Firmware up; static IP 192.168.1.50 mismatches host network 10.0.64.x. Needs DHCP or subnet-match. |
| usb_hid_device | 3 (USB-FS) | [BUG] Init fails | PC parked at usb_hid_panic_halt (main.c). USB init returns error on real silicon -- likely ra8_cgc_usbhs_pll_enable timeout or a stub that we promoted assuming chip behaviour that doesn't match. |

## Open follow-ups for next hardware session

1. **USB device bring-up debug** -- usb_hid_device fails. Add log/gdb-trace to identify exact failing call. Likely candidate: ra8_cgc_usbhs_pll_enable's USBCKCR PLL-lock wait may not actually settle in real hardware; or a missing pin-enable.
2. **Ethernet integration** -- switch threadx_lwip_tcp_echo to lwIP DHCP so it picks up an IP from any subnet the cable connects to. Or add a runtime config knob.
3. **Phase 7.1 tier 1 LED-only sweep** -- flash blink_hal, threadx_mpu_partition_demo (LED+MPU), confirm each runs.
4. **ra8_board_uart_console real-console verification** -- refactor uart_hello to call ra8_board_uart_console_* (now correct on SCI8) instead of raw ra8_sci_*.

## 2026-05-02 broader silicon sweep

Quick-flash + halt-and-check-PC across more example apps:

| App | Result | Halt PC location |
|---|---|---|
| blink | [PASS] | ra8_delay_ms loop |
| blink_hal | [PASS] | ra8_delay_ms loop |
| uart_hello | [PASS] | "hello, ra8d2!\n" stream verified |
| clock_check | [PASS] | ra8_delay_ms loop |
| threadx_blink | [PASS] | tx_thread_schedule idle |
| threadx_filex_demo | [PASS] | tx_thread_schedule idle |
| threadx_canfd_demo | [PASS] | tx_thread_schedule idle |
| threadx_ota_demo | [PASS] | tx_thread_schedule idle |
| threadx_lwip_tcp_echo | [PASS] runs, [WARN] unreachable (subnet) | tx_thread_schedule idle |
| threadx_mpu_partition_demo | [WARN] caught fault (intentional?) | internal_bkpt -- the deliberate cross-region access fired the fault handler as designed; the panic-spin is the demo's "fault caught" path |
| threadx_levelx_demo | [INVESTIGATE] still in main init at sample time | needs longer settle window |
| threadx_filex_levelx_demo | [INVESTIGATE] still in main init | needs longer settle |
| threadx_https_client | [INVESTIGATE] still in main init | needs longer settle |
| ra8_bootloader | [INVESTIGATE] still in system_init | needs longer settle |
| threadx_netx_tcp_echo | [BUG] ra8_error_handler panic | likely SCI8 console init racing -- same pattern as uart_hello pre-fix? |
| threadx_ipc_demo | [WARN] early sample mis-flagged as ra8_hw_err | retracted -- see "threadx_ipc_demo ra8_hw_err retraction" below; later sweeps (lines 122, 164) confirm it boots cleanly |
| usb_hid_device | [BUG] usb_hid_panic_halt | USB init returns error (suspected ra8_cgc_usbhs_pll_enable hang or missing pin enable) |

**Score: 9 of 17 sampled apps confirmed running on silicon. 4 still settling. 4 need bug fixes.**

## 2026-05-02 systematic 27-app sweep

Built a /tmp/ra8d2-hw-test test harness (flash + UART capture + halt-and-PC).
Results across 27 testable apps:

### [PASS] Confirmed running on silicon (12 apps)
- blink, blink_hal, clock_check, uart_hello (UART verified)
- threadx_blink, threadx_filex_demo, threadx_canfd_demo, threadx_ota_demo
- threadx_lwip_tcp_echo (boots, runs scheduler)
- threadx_filex_levelx_demo (boots, prints "[fxlx] booting...")
- threadx_ipc_demo (prints continuously -- M85 sends ping, no M33 reply
  because M33 has no firmware loaded -- this is the expected partial
  state, not a bug)
- usb_hid_device (after 394055f13 fix), usb_cdc_echo (after this commit)

### [INVESTIGATE] Partial / boot logs visible
- threadx_filex_levelx_demo: "[fxlx] formatting + opening LevelX
  partition / [fxlx] lx_nor_flash_format failed" -- XSPI NOR driver
  hand-off doesn't initialise the chip correctly
- threadx_levelx_demo: same lx_nor_flash_format path fails

### [BUG] Real bugs surfaced (not yet fixed)
1. **NetX malloc** -- threadx_netx_tcp_echo: nx_system_initialize calls
   malloc -> ra8_sbrk_trap fires (NASA Rule 3 enforcement). Need to
   compile NetX with NX_TRACE_INSERT off + NX_PHYSICAL_HEADER right or
   pre-allocate a static heap pool.
2. **XSPI NOR flash format** -- LevelX apps fail at lx_nor_flash_format.
   Likely the lx_nor_driver_ra8_xspi_initialize hook isn't actually
   bringing up the IS25LX512M-JHLE chip. Needs xSPI bring-up debug.
3. **USB device enumeration silent** -- usb_hid_device firmware runs
   stably but macOS never enumerates VID 0x1209. ra8_nsc_usb_init or
   downstream USB-FS controller bring-up missing a step (possibly D+
   pull-up, PHY clock, or VBUS-detect).
4. **USB host apps fault** -- usb_host_cdc_echo / keyboard / msc_browse
   all hit Default_Handler. USBHS host bring-up incomplete.
5. **BLE apps fault** -- threadx_nimble_peripheral prints "[nimble] boot"
   then crashes. Renesas BLE patch image (vendor blob) required for
   radio init; documented in VENDOR_BLOBS.md.
6. **threadx_https_client** -- stuck in main init at line 239. Needs
   network up (so depends on lwIP echo working too).
7. **ra8_bootloader** -- stuck in system_init.c. Boot stub design
   may intentionally halt waiting for a banked image; need to verify.

## 2026-05-02 final sweep (after ra8_rand_stub fix)

The xorshift32 rand() override (commit 6d2ebbfac) was a **massive
unblocker**. Apps that previously hit ra8_sbrk_trap fatal_error from
inside newlib's rand()/srand() now boot cleanly:

### [PASS] Confirmed running on silicon (18 of 27 apps)
- blink, blink_hal, clock_check, uart_hello
- threadx_blink, threadx_filex_demo, threadx_canfd_demo, threadx_ota_demo
- threadx_ipc_demo (M85 sends ping; M33 has no firmware so no reply -- expected)
- threadx_lwip_tcp_echo, threadx_netx_tcp_echo *(newly unblocked)*
- ethernet_tcp_echo *(newly unblocked)*
- usb_cdc_echo, usb_hid_device, usb_msc_device *(newly unblocked)*
- usb_host_cdc_echo, usb_host_keyboard, usb_host_msc_browse *(newly unblocked)*

### [BUG] Still failing (9 apps)
- **5 BLE apps** (ble_peripheral, threadx_nimble_peripheral, threadx_ble_central,
  threadx_ble_mesh_node) -- radio init panics. **Renesas BLE patch image required**
  (vendor blob, documented in VENDOR_BLOBS.md). Cannot fix without the blob.
- **2 LevelX/XSPI apps** (threadx_levelx_demo, threadx_filex_levelx_demo) -- both
  panic in lx_nor_flash_format. **XSPI NOR driver bring-up incomplete** for the
  IS25LX512M-JHLE chip on EK-RA8D2 v1.
- **threadx_https_client** -- stuck in main init line 239 (see
  "threadx_https_client RSIP BIST root cause" section below). Panic is in
  `demo_setup_or_halt()`, triggered by `ra8_rsip_init({.run_bist=true})`,
  not Ethernet.
- **threadx_mpu_partition_demo** -- ra8_error_handler spin. Probably the
  intentional cross-region fault firing as designed (the fault IS the demo).
- **ra8_bootloader** -- stuck in ra8_log.c. Log-loop blocking on UART; likely
  the boot stub's intentional "halt waiting for banked image" path.

## Summary

**Real bugs caught + fixed via hardware bring-up this session:**

| Commit | Bug | Impact |
|---|---|---|
| d3a9a278f | SysTick weak-alias dropped ra8_time.c handler | All 36 vector tables -- without fix every blink/delay app spins forever |
| 5f7110168 | BSP UART console wired to wrong SCI channel (3 vs 8) | All BSP UART users |
| 394055f13 | usb_hid_device panics on pre-enum send_report | Any USB device app with same break-on-error pattern |
| a953d16c9 | usb_cdc_echo same pattern | Same |
| 2ec83f594 | lwIP echo hardcoded subnet -> DHCP | Reachability on any network |
| **6d2ebbfac** | **rand() heap-allocates -> sbrk trap; xorshift32 override** | **5 apps unblocked: NetX, Ethernet, all USB device + host** |

**Audit-chain bugs also caught (not all hardware-related):**
- SDRAM base address (a98cbcb60), ELC CAN MRAM_ERI (cf1c8c70f), PFS PSEL (7cc37e1f9),
  RSIP key_op_status collision, HUM citations (446930c5a, b6f6227d5)

## 2026-05-02 hardware verification of recent fixes

Flashed each recently-touched app and read back PC + macOS USB tree
with the J-Link probe (serial in `.env` `JLINK_SN`).

| App | Outcome | Evidence |
|---|---|---|
| blink (regression baseline) | **PASS** | PC=`ra8_time.c` (ra8_delay_ms loop) |
| threadx_mpu_partition_demo | **PASS -- fix verified** | PC=`tx_timer_interrupt.S:237`, LR=`main.c` (worker thread). Pre-fix: HardFault |
| threadx_filex_levelx_demo | **FAIL -- code fix insufficient** | PC=`main.c` (`demo_panic_halt`), LR=`main.c` -- `lx_nor_flash_format` still returns non-LX_SUCCESS. Pin routing + DATASIZE fixes built clean but did not make the IS25LX512M actually format. Likely still missing: octal-DDR mode-switch, or RDID returning wrong manufacturer ID, or RESET timing |
| usb_hid_device | **FAIL -- does not enumerate** | Chip alive in `tx_thread_schedule.S:264`, but VID 0x1209 absent from macOS `ioreg -p IOUSB`. USBX library is wired but no host-visible device |

### USB enumeration root cause (now confirmed by hardware)

`port/usbx/src/ux_dcd_ra8_usb.c::ux_dcd_ra8_usb_initialize` calls
`ra8_usb_attach_handler(internal_event_cb, nullptr)` -- so the DCD
bridge has a callback waiting for events. But that callback only
fires when something invokes `ra8_usb_dispatch(speed)`. Grep across
the entire repo confirms **no source file** ever calls
`ra8_usb_dispatch` from an IRQ context (it is linked into every
binary because `ra8_usb.c` defines it, but it has zero callers).

Result: the USB controller's `INTSTS0` accumulates SETUP/CTRT bits
that nobody reads, so SETUP packets time out at the host before
the DCD ever sees them. macOS gives up enumerating.

**Fix needed** (separate task): in `ux_dcd_ra8_usb_initialize`, call
`ra8_isr_register(k_ra8_event_usbfs_int, slot, internal_usb_isr,
prio, ctx)` (and likewise USBHS for HS-mode), where
`internal_usb_isr` is a thin C function that calls
`ra8_usb_dispatch(speed)`. The ELC event codes for `usbfs_int` /
`usbhs_int` need to be added to `ra8_elc_regs.h::ra8_elc_event_t`.

## 2026-05-02 second-round hardware verification

After USBX IRQ wiring (commit 5b65e45b9, since reverted) and xSPI
octal bring-up extensions (commit 52e373507):

| App | Outcome | Evidence |
|---|---|---|
| usb_hid_device with IRQ wiring | FAIL HardFault | PC=0xEFFFFFFE, MMFAR/BFAR=0x40700004 (out of any peripheral range). Faults the moment ra8_isr_register(USBFS_INT) is called. The ELC event code 0x09A taken from FSP for older RA series is almost certainly wrong for RA8D2 |
| usb_hid_device polled (post-revert in commit a67acbc26) | PASS chip-alive, FAIL enumeration | PC=tx_thread_schedule.S:268. SYSCFG=0x411 (USBE+DPRPU+SCKE), INTSTS0=0x9F00 ticking, but VID 0x1209 still absent from macOS ioreg. Polling cadence (~30ms via jiggle period) is too slow for SETUP-window timeouts |
| threadx_filex_levelx_demo with all xSPI fixes | FAIL same panic site | PC=main.c (panic_halt), LR=main.c, i.e. lx_nor_flash_format still returns non-LX_SUCCESS even after CMDCMP poll budget bump (64 -> 1M), tPUW reset wait (1ms -> 15ms), BMCTL0 disable, and RDID validation. UART won't drain the failure log (1-3 bytes captured at any baud) so the actual RDID response is not yet observable |

### Confirmed via JLink memory read -- round 2 (after dual-protocol soft-reset)

`g_ra8_xspi_rdid_observed` (8 words) at `0x2200448C` now reads `{magic, 1, 0, 0xFFFFFF, 0, 0, 6, 0}`:
- magic = 'RDID', call_count = 1, rid_err = `k_ra8_ok`
- jedec_id still `0x00FFFFFF` (chip silent)
- reset_8d_err = 0 (8D soft-reset controller-side OK)
- reset_1s_err = 0 (1S soft-reset controller-side OK)
- stage = 6 (`k_ra8_xspi_stage_rdid` -- reached RDID step)

**Hypothesis #3 (stuck-in-8D) ruled out.** Controller is healthy (CMDCMP fires for every command). Remaining: pin routing in `s_xspi_octa_pins[]` is wrong on at least one of the 12 OCTA pins, OR DQS is not being clocked back. This is logic-analyzer-class debugging now -- beyond what JLink + UART can resolve.

### Confirmed via JLink memory read (commit 2f2560915 + first hardware capture)

`g_ra8_xspi_rdid_observed` at `0x2200448C` reads `{0x44494452, 1, 0, 0x00FFFFFF}`:
- magic = 'RDID' (probe ran)
- call_count = 1 (priv_bus_init_once executed once)
- rid_err = 0 (`ra8_xspi_flash_read_id` returned `k_ra8_ok`)
- jedec_id = **0x00FFFFFF** -- expected `0x009D5A1A` for IS25LX512M

The all-ones response is the diagnostic floor: **the chip is not responding at all**. The xSPI controller successfully clocks out the RDID opcode and the response window completes without timeout, but the data lines come back floating high. Either:
- Chip is still in reset (RESET_L not actually driven high for tPUW)
- One of the 12 OCTA pins (CS / CK / DQS / DQ0..DQ7) is mis-routed in PSEL -> chip doesn't see CS asserted, so it never drives DQ
- Chip is in a different protocol mode than the controller (chip ships in 1S-1S-1S; if a prior boot put it in 8D-8D-8D and we don't switch back via SRESET, the chip ignores 1S commands)

### Open WIP

1. USBFS_INT ELC event code for RA8D2 -- verify the actual code from the RA8D2-specific FSP bsp_elc.h (currently 0x09A is suspected wrong).
2. IS25LX512M RDID actual response - UART would not drain in time to
   print the `[LX_XSPI] RDID returned 0xNNN` log line before
   `demo_panic_halt`. Diagnostic now stashed in SRAM via
   `g_ra8_xspi_rdid_observed` (defined in
   `port/levelx/src/lx_nor_driver_ra8_xspi.c`). Read it with JLink after
   the panic halt:

   1. Look up the global's address for the app under test:

          arm-none-eabi-nm examples/ek_ra8d2/threadx_filex_levelx_demo/build/threadx_filex_levelx_demo.elf | grep g_ra8_xspi_rdid_observed
          arm-none-eabi-nm examples/ek_ra8d2/threadx_levelx_demo/build/threadx_levelx_demo.elf | grep g_ra8_xspi_rdid_observed

      Current addresses (will move with code changes):
        - threadx_filex_levelx_demo: `0x2200448c`
        - threadx_levelx_demo:       `0x22001d28`

   2. From `JLinkExe` (after `connect`, halt the target) run:

          mem32 0x2200448c 4

      Four words are dumped in this order:
        - `[0] magic`      = `0x44494452` ("RDID" little-endian) once
                             written; `0` means the probe never ran.
        - `[1] call_count` = number of `priv_bus_init_once` probe
                             attempts since reset.
        - `[2] rid_err`    = `ra8_err_t` from `ra8_xspi_flash_read_id`
                             cast to uint32 (`0` = `k_ra8_ok`).
        - `[3] jedec_id`   = packed `(mfr<<16)|(type<<8)|capacity`;
                             expected `0x009D5A1A` for IS25LX512M-JHLE.

      Replace the address with the value from `nm` for the app you
      flashed.
3. USB polled-mode tick rate -- even if the IRQ event code is wrong, a thread-pumped ra8_usb_dispatch at >= 1 kHz should keep up with FS enumeration.

## 2026-05-02 MPU demo HardFault root cause + fix

`threadx_mpu_partition_demo` was previously documented as "intentional
fault catch". Wrong -- there is no deliberate cross-region access in
the demo; it just blinks LED1 from a ThreadX worker thread inside an
MPU-protected region. The observed `internal_bkpt` PC is a real
HardFault.

Root cause: `s_mpu_cfg.mair0 = 0`, so `attr_idx 0` resolved to
device-nGnRnE (strongly-ordered) for all three regions. The MRAM
region (0x02000000, 1 MiB) holds executable code; instruction fetches
from device-typed memory are UNPREDICTABLE per Armv8-M ARM D1.6.7,
and the M85 HardFaults on the first fetch after MPU enable.

Fix (commit `c41954c8f`): set MAIR0 attr 0 = 0xFF (Normal, inner+outer
write-back, RW-allocate, non-transient) so MRAM/SRAM are Normal
memory; route the peripheral region to attr 1 = 0x04 (device-nGnRnE).

## 2026-05-02 ra8_bootloader spin is intentional

`examples/ek_ra8d2/ra8_bootloader/main.c` is `while(1) { wfi; }` reached when
both bank A and bank B fail `internal_bank_is_valid` -- i.e. neither
slot holds an app. In the test environment we never flash apps into
bank A/B, so the spin is the design-intended terminal state. The
earlier "stuck at ra8_log.c" observation was just a sample during
the log burst before the spin started. No bug, no fix needed.

## 2026-05-02 USB device enumeration root cause

Investigated why macOS never enumerates `usb_hid_device` despite SYSCFG showing
USBE=1, DPRPU=1, SCKE=1 and INTSTS0 ticking on the bus.

**Finding:** `ra8_usb_dispatch` (the function that reads INTSTS0 and forwards
SETUP/BRDY/CTRT events to a registered handler) is **never called by anything**:
- No `ra8_isr_register` for USBFS_INT in `libs/ra8_hal/src/ra8_usb*.c`
- No example main loop polls `ra8_usb_dispatch`
- `ra8_usb_phid_handle_setup` exists but has no caller -- only class SETUP is
  wired; standard SETUP (GET_DESCRIPTOR/SET_ADDRESS/SET_CONFIGURATION) has no
  handler at all

**Scope:** Implementing the standard-request enumeration state machine is a
multi-day port (FSP `r_usb_pdriver.c` + `r_usb_pstd_*` ~ 2000 LOC) and requires
either an IRQ wire-up or a polled tick from every USB-device example. Out of
scope for this hardware bring-up session; tracked as a separate effort.

**Implication for current state:** The 6 USB apps marked "running on silicon"
above are accurate as-stated (firmware boots, no fault, main loop alive) but
**none of them actually enumerate as USB devices/hosts on the bus**. They are
"firmware-up" not "USB-functional".

## 2026-05-02 threadx_https_client RSIP BIST root cause

Investigated why `threadx_https_client` halts at `main.c` with **zero UART
output** (`/tmp/ra8d2-hw-test/runs/threadx_https_client.uart` is 0 bytes).

### Evidence chain

1. `arm-none-eabi-addr2line` confirms PC=0x02000368 maps to `demo_panic_halt`
   at `examples/_unsupported/threadx_https_client/main.c` -- the `wfi` inside the
   panic-spin, not the actual fault site.
2. `demo_panic_halt()` is called from 7 sites: 6 inside `demo_setup_or_halt()`
   (lines 258, 261, 264, 267, 270, 278) and one at line 925 (post
   `tx_kernel_enter` return). The boot banner `[https] booting ThreadX...`
   is printed at line 919, **after** `demo_setup_or_halt()` returns.
3. **No banner ever appears on the UART** -> the panic happened inside
   `demo_setup_or_halt()`, before line 919.
4. The only call in `demo_setup_or_halt()` that is **unique to this app**
   (i.e. not also exercised by the apps that DO boot) is
   `ra8_rsip_init({.run_bist=true})` at line 277. Apps `threadx_lwip_tcp_echo`,
   `threadx_netx_tcp_echo`, and `ethernet_tcp_echo` share lines 257-270
   (cgc/time/uart/ethernet init) and they all reach their banner.

Conclusion: the panicking call is line 278, fired by
`ra8_rsip_init` returning non-`k_ra8_ok`.

### Why ra8_rsip_init fails on real silicon

`libs/ra8_hal/src/ra8_rsip.c::internal_run_bist()` does:

```c
*ctrl   |= k_ra8_rsip_mask_ctrl_bist;     /* arm BIST */
*status |= k_ra8_rsip_mask_status_bistok; /* off-target hack */
return internal_wait_bit(k_ra8_rsip_off_status,
                         k_ra8_rsip_mask_status_bistok); /* spin */
```

The OR-write into `STATUS` is described in the source comment as a
"no-op on silicon" off-target hack so the unit test deterministically
terminates the spin without modelling the BIST sequencer. On the
RA8D2, the `STATUS.BIST_OK` bit is set by the access-management
circuit (AMC) inside the sealed RSIP-E engine, **not** by the host
write. If the RSIP-E AMC firmware is not actually running -- which is
the default state until the FSP-equivalent SCE init sequence has
loaded the AMC code page -- the bit never sets, `internal_wait_bit`
exhausts its 4096-iteration budget, and `internal_run_bist` returns
`k_ra8_err_hw_init_failed`. `ra8_rsip_init` propagates the error and
`demo_setup_or_halt()` jumps straight to `demo_panic_halt()`.

The hand-rolled CTRL/STATUS register layout in
`libs/ra8_hal/inc/ra8_rsip_regs.h` (CTRL @ +0x0, STATUS @ +0x4,
ENABLE/RESET/BIST bit assignments) is **not documented in the RA8D2
Hardware User's Manual**. RSIP-E (a.k.a. SCE9) is a sealed-engine
peripheral whose only Renesas-supported entry point is the FSP
`r_sce_*` driver family. The current `ra8_rsip` register definitions
were inferred for the off-target test path and have never been
validated against silicon.

### Fix scope

Bringing real RSIP/SCE up requires:

1. Importing the FSP `r_sce` AMC firmware blob into the tree (a
   binary lifecycle / key-injection page that ships only via FSP).
2. Either translating `r_sce_subprc_select` + the SCE9 bring-up
   handshake into our HAL, or wrapping the FSP driver as a
   third-party blob behind a thin `ra8_rsip_*` shim.
3. Re-validating CTRL/STATUS/CMD/MAILBOX register offsets against
   FSP's `bsp_sec.h` (the canonical layout) instead of the inferred
   off-target layout.

This is a multi-day port comparable in scope to the USB enumeration
work (~2000 LOC of vendor driver state machine plus a binary blob
lifecycle). **Out of scope for the bring-up sweep; tracked here as
the canonical root cause for `threadx_https_client`'s line-239 halt.**

### Workaround for unblocking the HTTPS demo separately

For functional testing of the NetX-Duo + Mbed TLS data path without
touching RSIP, the demo could be patched to:

- Replace `ra8_rsip_init({.run_bist=true})` with a no-op on hardware
  builds (the call is currently the only one at boot), and
- Replace the `ra8_rsip_trng_read`-backed
  `mbedtls_psa_external_get_random` with the existing xorshift32
  `rand()` (already in tree per commit 6d2ebbfac) until SCE comes up.

That would prove the network/TLS stack independently of crypto
acceleration, but produces cryptographically weak entropy and is
deliberately NOT being committed -- it would mask the real bug. Doc
only.

## 2026-05-02 threadx_ipc_demo ra8_hw_err retraction

The "broader silicon sweep" table above (line 106) flagged
`threadx_ipc_demo` as "ra8_hw_err fired -- needs investigation". After
re-tracing the demo's code paths and re-reading the two later sweep
results in this file, that initial observation was a sampling artifact,
not a real bug. The demo runs cleanly and the entry has been amended.

### Code-path audit

`examples/ek_ra8d2/threadx_ipc_demo/main.c` has exactly one halt path:
`ipc_demo_panic_halt()` (line 154), reachable only from
`ipc_demo_setup_or_halt()` and from `tx_thread_create` failure inside
`tx_application_define`. Every site that can call it is local to the
M85 boot path:

- `ra8_cgc_init` -- shared with every booting app, would fail uniformly.
- `ra8_cgc_get_clock_hz(cpuclk0|pclka)` -- same.
- `ra8_time_init(cpuclk0_hz)` -- same.
- `ipc_demo_pins_init()` (PD_02 / PD_03 -> SCI8 async) -- same pin
  routing the verified-working `uart_hello` uses.
- `ra8_sci_init(8, &sci_cfg)` -- same SCI channel as the working UART
  apps.
- `ra8_ipc_channel_for_send(cpu0, 0, ...)` and
  `ra8_ipc_channel_for_recv(cpu0, 0, ...)` -- pure computation that
  validates `core <= cpu1` and `pair <= 1`; both arguments are compile-
  time constants in range, so these cannot fail.
- `ra8_ipc_init(&send_cfg)` / `ra8_ipc_init(&recv_cfg)` -- writes
  `CLR.RST` and the IRQ/RERR/FERR clear mask to the channel's `CLR`
  register, then stores per-channel state. IPC has no `MSTPCR` gate
  (HUM Ch 3 -- always-on CPU-bus peripheral), and channels 0 and 2 are
  always reachable on the M85 side regardless of M33 state. With both
  `cfg.channel == 0` and `cfg.channel == 2` (in range), the function
  cannot return anything but `k_ra8_ok`.

### Steady-state behaviour without an M33 image

Inside the worker thread (`ipc_demo_thread_entry`):

- `ra8_ipc_send_message_retry(channel=2, payload=ping, retries=16)`
  writes `TXD` for the M85->M33 FIFO. The 4-stage hardware FIFO (HUM
  Ch 3.1 p 204) accepts up to 4 unread words and then reports
  `STA.FULL`; the retry helper returns `k_ra8_err_hw_timeout` (NOT a
  panic) and the demo logs `[ipc_demo] send err\r\n`.
- `ra8_ipc_recv_message(channel=0, &word)` returns `k_ra8_err_no_data`
  whenever `STA.RDY == 0`; the loop falls through and logs
  `[ipc_demo] <no reply>\r\n`.

Neither path calls `ra8_error_handler`, `ra8_panic`, or any halting
helper. The demo is structurally tolerant of "M33 firmware not
loaded": it prints a pong-or-no-reply line every second forever and
never asserts.

### Reconciling the two earlier observations

- "broader silicon sweep" (line 106) sampled the chip very shortly
  after `JLinkExe loadfile` + `g; sleep ?; halt` and reported a
  transient state. Most plausibly the sample landed during the
  CGC/PLL settle window (PLL1 lock + cache enable) where the CPU is
  briefly executing inside `ra8_cgc_init` -- the gdb backtrace at that
  PC does not look like a clean main-loop sample to the table-builder
  script and got bucketed as `ra8_hw_err`.
- "systematic 27-app sweep" (line 122) and "final sweep" (line 164)
  both used a longer settle window and the on-board UART capture, and
  both observed continuous output -- the demo was running normally.

### Disposition

- No code change. The demo is correct as shipped and tolerates the
  absent M33 image by design (see `main.c` lines 38-49 of the file
  header doxygen, and the `<no reply>` log path).
- The line-106 table entry is amended to reflect the retraction.
- An M33 image to close the ping/pong loop is explicitly out of scope
  per the project roadmap (no M33 build infrastructure in this tree).

## 2026-05-02 evening sweep (post-test-infrastructure work)

Full smoke sweep of every app under `examples/ek_ra8d2/` (the EVM tier).
Probe: on-board J-Link OB (`.env` `JLINK_SN`) -> EK-RA8D2 v1, JLinkExe v9.38a.
Per-app procedure (halt-PC classification, executed manually -- the
since-retired developer-laptop smoke harness hung in this environment):

1. `bash scripts/dev/flash.sh examples/ek_ra8d2/<app>/build/<app>.hex` (30s
   timeout per flash).
2. `sleep 5` to let init code settle.
3. `JLinkExe` script: `device R7KA8D2KF_CPU0; si 1; speed 4000;
   connect; halt; regs; q`.
4. `arm-none-eabi-addr2line -f -e <elf> 0x<PC>`.
5. Classify against the same PASS / WIP / FAIL / UNKNOWN rubric the
   harness uses.

Build pass count: 26 of 26 apps (`make <app>` clean for every EVM-tier
app). Hardware results below.

| App | Result | PC | Symbol |
|---|---|---|---|
| blink | PASS | 0x02000B2E | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| blink_hal | PASS | 0x02000B8A | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| clock_check | PASS | 0x02000BEE | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| ereader | WIP | 0x0200042C | ereader_panic_halt examples/ek_ra8d2/ereader/main.c |
| ethernet_tcp_echo | PASS | 0x020018AE | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| lcd_demo | WIP | 0x02000204 | lcd_demo_panic_halt examples/ek_ra8d2/lcd_demo/main.c |
| ra8_bootloader | UNKNOWN | 0x02000530 | SystemInit examples/ek_ra8d2/ra8_bootloader/system_init.c |
| threadx_blink | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_canfd_demo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_filex_demo | PASS | 0x02000F74 | ra8_time_on_tick libs/ra8_core/src/ra8_time.c |
| threadx_filex_levelx_demo | WIP | 0x0200035C | demo_panic_halt examples/ek_ra8d2/threadx_filex_levelx_demo/main.c |
| threadx_ipc_demo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_levelx_demo | WIP | 0x02000326 | demo_panic_halt examples/ek_ra8d2/threadx_levelx_demo/main.c |
| threadx_lwip_tcp_echo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_mpu_partition_demo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_netx_tcp_echo | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |
| threadx_ota_demo | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |
| threadx_usbx_cdc_demo | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |
| uart_hello | PASS | 0x02000CAA | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_cdc_echo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| usb_hid_device | UNKNOWN | 0x02002DD0 | ux_dcd_ra8_usb_irq port/usbx/src/ux_dcd_ra8_usb.c |
| usb_host_cdc_echo | PASS | 0x0200117E | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_host_keyboard | PASS | 0x02001156 | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_host_msc_browse | PASS | 0x0200142E | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_msc_device | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |

### Tally

- Total apps swept: 26
- PASS: 20
- WIP: 4 (ereader, lcd_demo, threadx_filex_levelx_demo, threadx_levelx_demo)
- UNKNOWN: 2 (ra8_bootloader, usb_hid_device)
- FAIL: 0
- NOBUILD: 0

### Notes (no source changes; record-only per task scope)

- The two `UNKNOWN` rows are not faults. `ra8_bootloader` halt PC sits in
  `system_init.c`, consistent with the documented intentional spin
  when neither bank A nor bank B holds a valid image (see "ra8_bootloader
  spin is intentional" section above). `usb_hid_device` halted inside
  `ux_dcd_ra8_usb_irq` (port/usbx/src/ux_dcd_ra8_usb.c), i.e. an active
  USB ISR on the M85 -- chip is alive and servicing interrupts. Neither
  matches the harness PASS keyword set, so they fall through to UNKNOWN.
- All four `WIP` rows reproduce previously documented init-failure
  panics: ereader and lcd_demo halt in their app-local
  `*_panic_halt`; the two LevelX/XSPI demos still bail in
  `demo_panic_halt` from `lx_nor_flash_format` (xSPI NOR bring-up
  remains incomplete -- see prior sections in this doc).
- Zero hard faults this sweep -- no `Default_Handler`, `HardFault_Handler`,
  or 0xEFFFFFFE lockups observed across all 26 apps.

## 2026-05-02 night sweep (post-USB-INTSTS0-fix + post-doxygen-avalanche)

Re-sweep of every app under `examples/ek_ra8d2/` after commit `3e522dd19`
(USB-FS DCD: stop clearing INTSTS0 before the bridge callback runs, and
restore DCPCTR.PID = BUF after every received SETUP token) and after the
test/doxygen avalanche that landed between the evening sweep above and
this run (`07b84847a` MC/DC vector subsets, `13e1e96a8` /
`2673557d5` doxygen audits in `ra8_hal`, `f0ceae6cf` MISRA rule-12.1
closure, etc.). Goal: confirm the USB-FS fix flipped `usb_hid_device`
into a host-visible enumeration AND that no other EVM app regressed
under the test/doc churn.

Probe: on-board J-Link OB (`.env` `JLINK_SN`) -> EK-RA8D2 v1, JLinkExe v9.38a.
Per-app procedure identical to the evening sweep (executed manually --
no automated sweep target in this environment):

1. `bash scripts/dev/flash.sh examples/ek_ra8d2/<app>/build/<app>.hex` (30s
   timeout per flash).
2. `sleep 5` (`sleep 8` for `usb_hid_device` so macOS has time to start
   the enumeration handshake) to let init code settle.
3. `JLinkExe` -- `device R7KA8D2KF_CPU0; si 1; speed 4000; connect;
   halt; regs; q`.
4. `arm-none-eabi-addr2line -f -e <elf> 0x<PC>`.
5. Classify against the same PASS / WIP / FAIL / UNKNOWN rubric the
   harness uses.

Build pass count: 26 of 26 apps clean (`make <app>` for every EVM-tier
app re-run from scratch this session). Hardware results below.

| App | Result | PC | Symbol |
|---|---|---|---|
| blink | PASS | 0x02000B2E | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| blink_hal | PASS | 0x02000B8A | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| clock_check | PASS | 0x02000BEE | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| ereader | WIP | 0x0200042C | ereader_panic_halt examples/ek_ra8d2/ereader/main.c |
| ethernet_tcp_echo | PASS | 0x020018AE | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| lcd_demo | WIP | 0x02000204 | lcd_demo_panic_halt examples/ek_ra8d2/lcd_demo/main.c |
| ra8_bootloader | UNKNOWN | 0x02000452 | internal_write32 examples/ek_ra8d2/ra8_bootloader/system_init.c |
| threadx_blink | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_canfd_demo | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |
| threadx_filex_demo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_filex_levelx_demo | WIP | 0x0200035C | demo_panic_halt examples/ek_ra8d2/threadx_filex_levelx_demo/main.c |
| threadx_ipc_demo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_levelx_demo | WIP | 0x02000326 | demo_panic_halt examples/ek_ra8d2/threadx_levelx_demo/main.c |
| threadx_lwip_tcp_echo | PASS | 0x020002A4 | __tx_ts_wait tx_thread_schedule.S:294 |
| threadx_mpu_partition_demo | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |
| threadx_netx_tcp_echo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| threadx_ota_demo | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |
| threadx_usbx_cdc_demo | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |
| uart_hello | PASS | 0x02000CAA | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_cdc_echo | PASS | 0x0200029A | __tx_ts_wait tx_thread_schedule.S:264 |
| usb_hid_device | UNKNOWN | 0x02001C8C | ra8_usb_fs libs/ra8_hal/inc/ra8_usb_regs.h |
| usb_host_cdc_echo | PASS | 0x0200117E | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_host_keyboard | PASS | 0x02001156 | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_host_msc_browse | PASS | 0x0200142E | ra8_delay_ms libs/ra8_core/src/ra8_time.c |
| usb_msc_device | PASS | 0x020002A0 | __tx_ts_wait tx_thread_schedule.S:268 |

### Tally

- Total apps swept: 26
- PASS:    20
- WIP:     4 (ereader, lcd_demo, threadx_filex_levelx_demo, threadx_levelx_demo)
- UNKNOWN: 2 (ra8_bootloader, usb_hid_device)
- FAIL:    0
- NOBUILD: 0

Identical PASS/WIP/UNKNOWN/FAIL counts to the evening sweep -- the
test-infrastructure / doxygen / MISRA churn between the two runs did
not regress any of the 20 PASSing apps and did not introduce any new
hard faults.

### USB-FS enumeration verification (commit `3e522dd19`)

`usb_hid_device` was flashed with the new INTSTS0/PID-BUF fixes and
allowed to settle for 8s before halt-and-read. Outcome:

- macOS host enumeration count for VID 0x1209 (`ioreg -p IOUSB -l |
  grep -c '"idVendor" = 4617'`): **0** -- the device still does not
  appear on the host USB tree.
- Halt PC = `0x02001C8C` -> `ra8_usb_fs (libs/ra8_hal/inc/ra8_usb_regs.h)`,
  i.e. the M85 is alive and parked inside the USB-FS register accessor
  invoked from the dispatch poll loop. No fault, no panic_halt.
- `g_ra8_usb_dcd_diag` at `0x2200564C` (struct size 0x68, dumped with
  `mem32 0x2200564C 26`):

```
2200564C = 44434455 00000000 00000001 00000000   <- magic | ctrt_count | dvst_count | setup_count
2200565C = 00000000 00000000 000000D0 00000040   <- dispatch_ok | dispatch_err | last_intsts0 | last_dvsq/ctsq
2200566C = 00000000 00000000 00000000 00000000   <- start of SETUP ring (8 x uint64 wire-order)
2200567C = 00000000 00000000 00000000 00000000
2200568C = 00000000 00000000 00000000 00000000
2200569C = 00000000 00000000 00000000 00000000   <- end of SETUP ring
```

Decoded:

- magic        = `0x44434455` (`UCDD` little-endian -- struct
                 initialized, bridge code linked in and ran).
- ctrt_count   = 0      -- **zero CTRT (CTRL-transfer-ready) edges seen**.
- dvst_count   = 1      -- one DVST (device-state change) edge seen
                          (the initial detached -> powered transition
                          when DPRPU was asserted).
- setup_count  = 0      -- no SETUP packet ever latched into
                          USBREQ/USBVAL/USBINDX/USBLENG.
- dispatch_ok  = 0
- dispatch_err = 0      -- chapter-9 dispatcher never invoked because
                          no SETUP arrived.
- last_intsts0 = `0x00D0` -- exactly the post-fix value documented in
                             the commit message (`SOFR | DVST | CTSQ`
                             status bits with all the W0C handshake
                             bits already cleared).
- last_dvsq/ctsq = `0x40` -- DVSQ = `0x4` (DEFAULT, i.e. POWERED but
                             not yet ADDRESSED) shifted into bits
                             [6:4]; CTSQ low nibble = 0 (idle).
- SETUP ring (16 words / 8 packets) = all zeros.

**Interpretation:** the INTSTS0/PID-BUF fixes from `3e522dd19` are
running on silicon (struct magic written, INTSTS0 settled to 0x00D0
exactly as the commit message predicted, DVST edge correctly counted
once), but they do not unblock enumeration on their own. The device
attaches electrically (DVSQ reaches DEFAULT) and macOS notices the
attach (the kernel does send SOF), but no SETUP token is ever
delivered to the controller -- ctrt_count and setup_count remain at
zero throughout the 8s settle window. This matches the commit
message's own caveat ("dispatch is being called only after the host
has already given up and electrically suspended the port") and
points at the init-order / DPRPU-timing follow-up the commit
explicitly defers ("the dispatch loop should start polling BEFORE
ra8_usb_device_attach asserts DPRPU, not after").

Net result: USB enumeration is still **not** functional, but the
two specific bugs `3e522dd19` fixed are no longer reachable on the
SETUP path -- INTSTS0 is no longer cleared pre-callback (last_intsts0
shows live status bits), and DCPCTR PID restoration is correctly
gated by a SETUP edge that simply never arrives. Out of scope to
diagnose further per task brief.

### xSPI RDID re-check (`threadx_filex_levelx_demo`)

`g_ra8_xspi_rdid_observed` at `0x2200448C` (8 words, dumped with
`mem32 0x2200448C 8`):

```
2200448C = 44494452 00000001 00000000 00FFFFFF   <- magic | call_count | rid_err | jedec_id
2200449C = 00000000 00000000 00000006 00000000   <- reset_8d_err | reset_1s_err | stage | (pad)
```

Decoded:

- magic        = `0x44494452` (`RDID`, probe ran).
- call_count   = 1     -- `priv_bus_init_once` executed exactly once.
- rid_err      = 0     -- `ra8_xspi_flash_read_id` returned
                          `k_ra8_ok` (controller-side OK).
- jedec_id     = `0x00FFFFFF` -- **chip still silent, identical to
                                 the evening sweep and to all prior
                                 captures**. No change.
- reset_8d_err = 0
- reset_1s_err = 0
- stage        = 6     -- `k_ra8_xspi_stage_rdid` reached.

No regression and no progress on the IS25LX512M-JHLE bring-up --
the RDID response is still all-ones, identical to the
"Confirmed via JLink memory read -- round 2" capture above.
Logic-analyzer-class debug needed; out of scope for this sweep.

### Notes (no source changes; record-only per task scope)

- All 20 PASS rows are byte-identical PC + symbol matches to the
  evening sweep, modulo the `usb_hid_device` row (now parked inside
  the USB-FS register accessor instead of `ux_dcd_ra8_usb_irq`,
  consistent with the new dispatch-poll loop introduced by
  `3e522dd19`).
- `ra8_bootloader`'s UNKNOWN row sampled inside
  `internal_write32` (system_init.c) this time instead of
  `SystemInit` itself -- still the same pre-banner clock/PFS
  bring-up burst before the documented intentional spin in
  `main.c`. Not a fault; matches the "ra8_bootloader spin is
  intentional" section above.
- `usb_hid_device`'s UNKNOWN row is the new INTSTS0/PID-BUF dispatch
  loop running cleanly -- it does not match the harness PASS keyword
  set (the new poll body is `ra8_usb_fs` accessor, not
  `tx_thread_schedule`/`ra8_delay_ms`/`main.c`/`internal_*`), so the
  classifier falls through to UNKNOWN. Chip is alive and the bridge
  IRQ ran (g_ra8_usb_dcd_diag.magic = `UCDD`).
- Zero hard faults across all 26 apps -- no `Default_Handler`,
  `HardFault_Handler`, `MemManage_Handler`, `BusFault_Handler`,
  `UsageFault_Handler`, `SecureFault_Handler`, or 0xEFFFFFFE
  observed.

## 2026-05-02 LevelX OCTA pin readback verification

Added a new JLink-readable global to the LevelX-on-xSPI driver
(`port/levelx/src/lx_nor_driver_ra8_xspi.c`), `g_ra8_xspi_pin_observed`,
which captures the 5-bit `PSEL` and the `PMR` bit of every OCTA
pin AFTER `ra8_board_xspi_pins_init` returns. Goal: confirm /
disprove the long-standing hypothesis that one of the 12 pins in
the BSP table (`s_xspi_octa_pins`) is mis-routed -- which would
silently drop CS / DQ on the wrong pad and explain the all-ones
RDID readout.

### Verification procedure

1. `arm-none-eabi-nm threadx_filex_levelx_demo.elf | grep g_ra8_xspi_pin_observed`
   -> address `0x220044AC` (will move; re-resolve on rebuild).
2. Flash, settle 5s, halt.
3. `JLinkExe ... mem32 0x220044AC 24` (12 rows x 8 bytes per row).

Each row encodes `{port, pin, psel_observed, pmr_observed, pfs_raw}`.

### Observed (commit pending)

```
220044AC = 011C0401 1C010002 011C0808 1C010000   <- CS=P104, CK=P808
220044BC = 011C0108 1C010002 011C0001 1C010002   <- DQS=P801, DQ0=P100
220044CC = 011C0308 1C010002 011C0301 1C010002   <- DQ1=P803, DQ2=P103
220044DC = 011C0101 1C010002 011C0201 1C010002   <- DQ3=P101, DQ4=P102
220044EC = 011C0008 1C010002 011C0208 1C010002   <- DQ5=P800, DQ6=P802
220044FC = 011C0408 1C010002 FFFFFFFF 00000000   <- DQ7=P804, sentinel
```

Decoded: every active row has `psel_observed = 0x1C`
(`k_ra8_psel_qspi`) and `pmr_observed = 1`. **All 12 OCTA pins are
correctly routed to the OSPI peripheral.** Pin-routing hypothesis
is conclusively ruled out. The BSP `s_xspi_octa_pins` enum matches
EK-RA8D2 v1 UM Table 29 (p 35) byte-for-byte, and the chip's PFS
hardware confirms the routing took effect.

The remaining all-ones RDID readout (`0x00FFFFFF`, captured via
`g_ra8_xspi_rdid_observed` at `0x2200448C`) is therefore caused by
something the chip is doing on the bus side -- not by the
controller failing to drive the right pad. Most likely candidates
are now (in order of plausibility):

1. **Voltage / pull-up issue on a DQ line** (would need a 'scope to confirm).
2. **DQS sampling phase wrong** (`LIOCFGCS[0].DQSEN` / `SDRSMPMD`).
3. **Chip stuck in QPI** (4S-4S-4S) -- the dual 8D + 1S reset
   sequence in `priv_bus_init_once` doesn't try the QPI-form RSTEN/RST.
4. **Octa-SPI PHY calibration** (`LIOCFGCS[0]` slew / drive strength)
   not yet matched to the IS25LX512M datasheet recommendations.

All four are logic-analyzer / 'scope class debugging. WIP marked
permanent for this app pair until that hardware is available.

## 2026-06-09 OSPI chip-select sweep + live-register evidence (issue #44)

Two new firmware-side hypotheses were tested on the bench with the
user confirming **SW4-1..8 are all physically OFF** (the Octo-SPI
layout). Both were ruled out, narrowing the fault to the bus/device
side conclusively.

### Chip-select sweep (CS0 vs CS1)

Hypothesis: the on-board Macronix flash hangs off the OSPI **CS1**
strobe (P104 = `om_0_cs1` in the FSP `ospi_b` pin map) but the driver
drove **CS0**, so the controller never selected the chip. Tested by a
temporary `flash_journal` sweep that re-inited the OSPI once per chip-select
(`LIOCFGCS[n]` + `CDCTL0.CSSEL`) and stamped the JEDEC ID each time:

```
g_fj_jedec_id (CS1) = 0x00FFFFFF
g_fj_jedec_cs0      = 0x00FFFFFF
g_fj_jedec_cs1      = 0x00FFFFFF
```

**Both chip-selects float identically.** Chip-select selection is NOT
the differentiator. (The sweep harness was reverted afterwards; the
production driver keeps its original CS0 default. Whether the flash
truly lives on CS0 or CS1 remains unconfirmed against the FSP pincfg,
but it does not matter while the bus floats on both.)

### Live OSPI controller registers (XSPI0 @ 0x40268000)

Read over SWD while the journal loop was issuing commands:

```
0x40268070 CDCTL0  = 0x00000008  <- CSSEL=1 selected, TRREQ self-cleared
0x40268080 CDBUF[0]= 0x00050021  <- last opcode (RDSR 0x05 status poll)
0x40268088 CDBUF[2]= 0x000000FF  <- data read back = 0xFF (line floats high)
0x40268184 COMSTT  = 0x00770000  <- per-CS ECS/INT monitors
0x40268190 INTS    = 0x00000000  <- CMDCMP cleared by driver INTC write
```

The decisive datum: **`CDCTL0.TRREQ` self-clears** -- the manual-command
engine *completes* every transfer (the controller clocks CS + opcode and
finishes), yet `CDBUF` data comes back `0xFF`. The controller is doing
its job; the flash simply never drives the data line.

### Conclusion

Combined with the 2026-05-02 pin-routing readback, the firmware side is
now fully exonerated: pins routed (PSEL=0x1C, PMR=1), RESET_L released
high with correct tRLRH/tRHSL timing, command engine completes on the
selected CS, and the U15 expander ACKs. The fault is **device/bus-side**
(power to U16, the SW4/level-shifter mux actually connecting the flash,
or DQ drive/pull) -- logic-analyzer / continuity-meter / 'scope class,
exactly as previously documented. No further blind firmware change is
justified; #44 stays blocked on physical instrumentation. Font storage
proceeds on the SD-card path instead.

## 2026-05-02 lcd_demo + ereader root cause

The `lcd_demo` and `ereader` apps both halt at `*_panic_halt` very
early in boot. `addr2line` of the captured PC + LR pinpoints:

| App | PC | LR | Failing call |
|---|---|---|---|
| lcd_demo | `0x02000204` (panic_halt) | `0x02000515` | `lcd_demo_pins_init()` (main.c) |
| ereader  | `0x0200042C` (panic_halt) | `0x02000657` | `ra8_board_led_init(k_ra8_board_led1)` (main.c) |

**Common root cause:** Both apps' `k_*_glcdc_pins[]` tables are the
unfinished TODO stub left over from before the EK-RA8D2 v1 board
manual landed in `docs/reference/`. They list LCDD0..LCDD7 as
P600..P607, LCDD8..15 as P700..P707, LCDD16..23 as P800..P807,
HSYNC/VSYNC/DE/CLK as P900..P903 -- a guess based on the chip's
GLCDC pin-capability table, not the actual board.

The real EK-RA8D2 v1 LCD-connector wiring (UM Table 33, p 42, the
"Parallel Graphics Expansion Port") is scattered across ports 2, 5,
6, 7, 8, 9, B with completely different bit assignments. Examples:

  - LCD_CLK   = P515  (BSP guess: P903)
  - LCD_TCON0 = P806  (BSP guess: P900 = VSYNC)
  - LCD_TCON1 = P805  (BSP guess: P901 = HSYNC)
  - LCD_TCON2 = P807  (BSP guess: P902 = DE)
  - RGB565 R5 = PB01
  - RGB565 R6 = PB04
  - RGB565 R7 = PB03
  ... etc, full table at HARDWARE_BRINGUP.md `Table 33` excerpt.

The downstream symptom differs between the two apps because of
init-order:

- `lcd_demo` calls `ra8_board_led_init(LED1)` BEFORE
  `lcd_demo_pins_init()`. LED1 is P600. The pin validator gives
  P600 to LED1, then `lcd_demo_pins_init()` immediately tries to
  re-claim P600 as LCDD0 (its first GLCDC pin) and gets
  `k_ra8_err_gpio_conflict`. Panic.

- `ereader` calls `ereader_pins_init()` BEFORE
  `ra8_board_led_init(LED1)`. The GLCDC route succeeds (P600 silently
  becomes LCDD0), then `ra8_board_led_init(LED1)` tries to take
  P600 as plain GPIO and the validator denies it. Panic.

`lcd_demo` also collides with LED2 (P303 not in its pin table) and
with the J-Link OB SCI8 console (PD02/PD03 -- LCDD0..LCDD7 stub
overlaps PD's range only if extended). `ereader`'s SDHI table
(`k_ereader_sdhi_pins`) is similarly stubbed at P400..P407, which
on EK-RA8D2 v1 are CANFD/IIC pads not SDHI -- the board has no
on-chip SD slot at all (Tier 7 in the README, requires extra
hardware).

### Fix scope

Properly fixing these two apps requires:

1. **Parallel Graphics Expansion Board 1** (sold separately by
   Renesas) physically attached to J1, OR concession that the
   apps are software-stack demos that exercise the GLCDC API
   without lighting an external panel.
2. Full re-typing of `k_lcd_demo_glcdc_pins[]` and
   `k_ereader_glcdc_pins[]` against UM Table 33's 28 entries.
3. Replacement of `k_ereader_sdhi_pins[]` with either real SDHI
   pads (port D / port C on RA8D2 -- requires datasheet pin-function
   sweep) or removal entirely if the EK-RA8D2 v1 has no SD slot.
4. Switching the LED-init order in both apps so the GLCDC + SDHI
   routing happens FIRST and the LED layer either uses pads not
   on the LCD bus, or drops LED indication entirely while the LCD
   is active.

Items 2-4 are mechanical but pointless without item 1. Both apps
are therefore reclassified as "requires Parallel Graphics
Expansion Board" and stay in the `WIP` column of the smoke
sweep until that hardware is available.

The LCD test vehicles that actually validate today (no expansion
board needed) are `clock_check`, `blink`, `blink_hal`,
`threadx_blink` -- none of which try to drive the LCD bus.

## USB-FS clock bring-up (PLL2 + USBCKCR) -- 2026-05-02

### Status

PLL2 brought up successfully. `ra8_cgc_pll2_enable(80, 0, /4)` runs
inside `ra8_cgc_usbfs_clock_enable` and is verified live on the chip:

```
mem 0x4001E03C 1 1   ; OSCSF
4001E03C = 68        ; bit 5 PLL1SF, bit 6 PLL2SF, bit 3 MOSCSF -- all set
```

Clock plan (EK-RA8D2, 24 MHz crystal):

```
XTAL 24 MHz / 2 (PL2IDIV) = 12 MHz
12 MHz * 80 (PLL2MUL)     = 960 MHz VCO   (silicon min, in spec)
960 MHz / 4 (PL2ODIVP)    = 240 MHz at PLL2P
240 MHz / 5 (USBCKDIVCR)  = 48.000 MHz    -- spec: 48 MHz +/- 0.25 %
```

Spec compliance: 0 ppm error -> PASS.

### Symptom

Despite PLL2 locking cleanly, host enumeration still fails:

```
ioreg -p IOUSB -l | grep -c '"idVendor" = 4617'
0
```

JLink readback after the panic-halt:

```
0x4001E03C OSCSF      = 0x68   ; PLL1SF=1, PLL2SF=1, MOSCSF=1 -- PLLs locked
0x4001E074 USBCKCR    = 0x40   ; USBCKSREQ=1, USBCKSRDY=0, USBCKSEL=0 (HOCO)
0x4001E06C USBCKDIVCR = 0x00   ; never reached -- still reset default /1
0x40250000 SYSCFG     = 0x00   ; USBFS module unclocked (MSTPB11 still gating)
0x40250040 INTSTS0    = 0x00   ; USBFS not running
```

The CPU is wedged in `demo_panic_halt()`, which means
`ra8_cgc_usbfs_clock_enable` returned `k_ra8_err_hw_timeout`. The
USBCKCR readback `0x40` proves the *first* register write
(`USBCKCR = USBCKSREQ` mask) executed, but `internal_wait_usbcksrdy(1U)`
never observed `USBCKSRDY = 1`.

### Hypothesis (next to investigate)

The USBCKCR.USBCKSRDY handshake on RA8D2 silicon may require the
USBFS module clock to already be ungated before the SREQ/SRDY
handshake can complete. The reset state has USBCKSEL = HOCO and
USBFS in MSTP-stop -- there is no clock running on the controller
for the gating logic to "stop" in response to USBCKSREQ = 1, so
USBCKSRDY may stay 0 indefinitely.

FSP runs the USBCKCR sequence inside `bsp_clock_init` *before*
any module is ungated, but FSP also has `BSP_CFG_UCLK_SOURCE`
default to HOCO, so the SREQ/SRDY path may only matter when the
caller is *changing* the source. Switching from a not-yet-running
HOCO selector to PLL2P may need either:

1. Touch `MSTPCRB.MSTPB11` to ungate USBFS *before*
   `ra8_cgc_usbfs_clock_enable`, so there's a clock for the
   gating logic to chase, or
2. Skip the SREQ/SRDY handshake on first programming (write
   USBCKDIVCR + USBCKCR = src directly with no SREQ bit), or
3. Start the HOCO explicitly (it might be HCSTP=1 at this point;
   `ra8_cgc_init` does not call `ra8_cgc_use_hoco`).

Diagnostic next step: read `MSTPCRB` (0x40087004) and `HOCOCR`
(0x4001E036) at the same halt-point to confirm which of (1) and
(3) apply. Then either pre-ungate MSTPB11 or skip the SRDY=1
wait when USBCKCR was 0 on entry.

### What this commit does

- Adds `ra8_cgc_pll2_enable(mul_int, mul_quarters, p_div)` with
  PLL2 stop -> programme -> start -> lock-poll sequence.
- Switches `ra8_cgc_usbfs_clock_enable` from PLL1Q/8 (41.67 MHz,
  out of spec) to PLL2P/5 (48.000 MHz, in spec).
- Wires `ra8_cgc_usbfs_clock_enable` into `usb_hid_device/main.c`
  (was previously declared but never called).
- Adds register accessors `ra8_sys_pll2cr / pll2ccr / pll2ccr2`
  and offset / encoding enums in `ra8_system_regs.h` and
  `ra8_cgc_regs.h`.

The 48 MHz reference now exists; the remaining work is the
USBCKSRDY handshake hypothesis above.

## 2026-06-09 OSPI JEDEC-ID bring-up: CDT command-byte left-justification fix + exhaustive bus-silence triage (#44)

Goal: make `flash_journal`'s `g_fj_jedec_id` (stamped via
`ra8_xspi_flash_read_id` right after `ra8_xspi_init`) read the Macronix
MX25UM25645G manufacturer ID `0xC2....` instead of the floating
`0x00FFFFFF`.

### Real firmware bug found + fixed: CDT CMD field was right-justified

`internal_make_cdt` (and the inline builder in
`internal_issue_reset_opcode`) placed a 1-byte opcode at `CDT[23:16]`
(`opcode << k_ra8_xspi_cdt_pos_cmd`, pos=16). The command-manual engine
transmits `CMDSIZE` bytes **MSB-first from bit 31**, so with `CMDSIZE=1`
it was clocking out the **zero** byte at `CDT[31:24]` and the chip saw
command `0x00` for every 0x9F / 0x05 / 0x06 / 0x02 / 0x20 op.

Confirmed against the FSP gold reference
`ra/fsp/src/r_ospi_b/r_ospi_b.c::r_ospi_b_direct_transfer`:

```c
cdtbuf0 |= (1 == command_length) ?
    ((command & 0xFF)   << 24) :   /* OSPI_B_PRV_CDTBUF_CMD_UPPER_OFFSET = 24 */
    ((command & 0xFFFF) << 16);    /* OSPI_B_PRV_CDTBUF_CMD_OFFSET       = 16 */
```

Fix: left-justify the opcode inside the 16-bit CMD field --
`cmd_shift = 8 * (2 - cmd_bytes)`, so a 1-byte opcode lands at
`[31:24]` and a 2-byte (8D complementary) pair fills `[31:16]`.
Verified on HW via a diagnostic global: the live `CDBUF[0].CDT`
went from `0x009F0061` to `0x9F000061` (opcode now in the high byte).
This is a genuine correctness bug that also affected RDSR/WREN/PP/SE
and the 1S software-reset path.

### Exhaustive controller-side triage -- chip still silent on the bus

With the CDT fix in place the chip **still** floats `0x00FFFFFF` in 1S.
Every firmware-controllable lever was swept on real silicon (J-Link
SWD, diagnostic `g_fj_probe[16]` array, completion flag in the top
byte: `0xC0......` = CMDCMP fired, `0x40......` = timeout):

| Lever swept | Result |
|---|---|
| CDT opcode at bit24 vs bit16 | both `C0FFFFFF` (CMDCMP fires, floats) |
| CSSEL=0 (P107/OM_0_CS0, routed in probe) vs CSSEL=1 (P104/OM_0_CS1) | both `C0FFFFFF` |
| LIOCFGCS CSMIN 0/2/8, CSASTEX, CSNEGEX | no change |
| LIOCFGCS SDRSMPMD (sample edge), SDRDRV, SDRSMPSFT 0..2 | no change |
| WRAPCFG DSSFTCS0 sample-shift 0/1/2/4 | no change |
| OCTASPICLK /1 (~8 MHz) vs /32 (~250 kHz) | no change |
| BMCTL0 = 0x00 vs 0x0C (FSP default) vs 0x11 vs 0xFF (RW all) | no change |
| Hardware reset: exact `RA8x1_Reset_OSPI.JLinkScript` P106 PFS pulse (0x40400858: 0x05 high / 0x04 low / 0x05 high) | no change |
| 1S software reset (RSTEN 0x66 / RST 0x99) | no change |
| 8D / 8S OPI reads (RDID 0x9F+0x60, 4-byte addr, dummy sweep 0..15) | `C0000000` (lines read low) or TRREQ wedge |
| Memory-mapped read @ `0x80000000` (OSPI0 area) | `FFFFFFFF`, no bus fault |

Signature: in 1S the DQ/SIO line floats **high** (0xFF, pull-up, chip
not driving); in 8D it reads **0x00**. CMDCMP retires every 1S transfer
cleanly. Controller is healthy end-to-end.

### Ruled out this session (with live register evidence)

- **Clock**: `OCTACKCR=0x01` (OCTACKSEL=MOCO, confirmed via FSP
  `bsp_clocks.h` `BSP_CLOCKS_SOURCE_CLOCK_MOCO=1`), `OCTACKDIVCR=0`
  (/1), `MOCOCR=0` (running), `OSCSF` MOCOSF=1 (stable). MSTPCRB
  bit16=0 (OSPI0 ungated). Slowing the clock 32x changed nothing.
- **Chip-select pins**: P104 PFS (`0x40400850`) reads `0x1C010002`
  (PSEL=0x1C, PMR=1) -- OM_0_CS1 routed. P107 (OM_0_CS0) routed in the
  probe; neither CS makes the chip answer.
- **SW4 mux**: per EK-RA8D2 v1 UM Table 3, Octo-SPI needs SW4-3 OFF
  (Octo-SPI Active) and SW4-4 OFF (Arduino/mikroBUS **Inactive** ->
  Octo-SPI prioritised; UM section 6.3 lines: "By default SW4-4 is off
  which disables connectivity to the Arduino headers, prioritizing
  Octo-SPI"). So **all-OFF is the correct OSPI configuration** -- the
  earlier "needs SW4-4 ON" reading was wrong. The U15 (PI4IOE5V6408 @
  I2C 0x43) override write succeeds on HW (`g_fj_expander_err=0`) and
  drives the all-OFF (= OSPI-active) pattern. Mux is in the right state.
- **CSMIN/sampling/BMCTL0/WRAPCFG**: all matched to FSP `R_OSPI_B_Open`
  defaults; none unblocks the read.

### Net

The CDT command-byte left-justification is a real, FSP-confirmed driver
fix and is committed. With it, the controller programs and clocks the
JEDEC read correctly (verified at the register level), but the on-board
MX25UM25645G does not drive its data lines under any controller
configuration reachable from firmware -- 1S floats high, OPI reads low,
memory-mapped reads float. Bus-side silence persists; capturing the
SCLK/CS/DQ waveforms is the next step to localise whether SCLK is
physically toggling at the flash pads.

## 2026-06-09 OSPI ROOT CAUSE FOUND: SW4-3 physically ON isolates the flash (#44)

The bus-side silence is now **root-caused with direct evidence**: the
physical **SW4-3 DIP switch is ON, which sets "Octo-SPI Inactive" and
electrically isolates the on-board flash (U3) from the MCU's OM_0 bus.**
No firmware change can read the flash while SW4-3 is ON -- the DQ/CK/CS
pins are disconnected from the part by the switch.

### Decisive measurement -- physical SW4 state read back over RIIC1

The U15 PI4IOE5V6408 expander (I2C 0x43, RIIC1) is wired across SW4 and
its input-level register 0x0F reflects the *physical* switch levels. A
diagnostic in `flash_journal` floated the expander port (IODIR 0x03 =
0x00 -> all inputs) and read reg 0x0F via `ra8_i2c_transfer`:

```
g_fj_exp_devid = 0x000000A0/A2  <- PI4IOE5V6408 answering (bus healthy)
g_fj_sw4_input = 0x000000F8     <- reg 0x0F, status byte 0x00 = read OK
```

`0xF8 = 1111_1000b`. Per the board polarity (**bit n HIGH = SW4-(n+1)
OFF; bit n LOW = SW4-(n+1) ON**), bit 2 (SW4-3) reads **LOW = ON =
Octo-SPI Inactive**. SW4-1/SW4-2 also read ON (bits 0,1 = 0); SW4-4..8
read OFF. This is the first time the physical DIP state was *measured*
rather than inferred -- it confirms the long-standing hypothesis that
the expander override cannot overpower the mechanical switch.

### Supporting evidence gathered this session

- **OPI 8D read times out on DQS, not just "reads 0x00":** an 8D-8D-8D
  RDID (LIOCFGCS=0xBFF: PRTMD=0x3FF + DDREN, opcode 0x9F60, addr4,
  DATASIZE4, LATE=4) was issued by raw J-Link CDBUF pokes. INTS came
  back **0x11 = CMDCMP | DSTOCS0** (bit 4, "DS Timeout for CS0":
  *data not received during the expected read phase*). The flash never
  toggles DQS because it is not on the bus -- consistent with isolation,
  not with a wrong OPI latency.
- **1S WREN->RDSR shows no WEL toggle:** after a clean firmware init,
  a 0x06 WREN then 0x05 RDSR (poked over SWD) retired CMDCMP but RDSR
  read back **0xFF** (floating pull-up), i.e. the WEL bit did not become
  1. A live chip would return 0x02. The chip does not respond in 1S.
- **xSPI instance + pin route verified CORRECT (rules out HYP-4):** the
  RA8D2 datasheet pin-function table maps every board OSPI pin to the
  **OM_0_ (instance 0)** silicon signal -- P104=OM_0_CS1, P107=OM_0_CS0,
  P106=OM_0_RESET, P100=OM_0_SIO0, P803=OM_0_SIO1, P800=OM_0_SIO5, etc.
  HUM PFS table: **PSEL 0x1C (11100b) = OSPI, OM_0_** functions. The
  driver targets instance 0 (XSPI0 @ 0x40268000) and the board routes
  PSEL=0x1C, both correct.

### EK-RA8D2 v1 UM internal contradiction (documentation defect)

The UM is self-contradictory on SW4-3, which has misled prior triage:

- **Table 3 (p 16):** "SW4-3 OFF = Octo-SPI Active" (and the Table 4
  conflict matrix marks SW4-3 OFF + SW4-4 ON as *invalid*, only
  consistent with OFF=Active).
- **Section 6.3 prose (p 35):** "The Octo-SPI NOR Flash can be isolated
  from the MCU bus by turning SW4-3 *off*."

Table 3 + the conflict matrix + the FSP `ospi_b` example + this repo's
expander polarity all agree: **OFF = Active, ON = Inactive/Isolated.**
The Section 6.3 prose is the error (stale copy from an older board manual).
The on-board flash is also listed as ISSI **IS25LX512M-JHLE** (mfr
**0x9D**) in UM Table 29 -- note the JEDEC mfr is 0x9D, not the Macronix
0xC2 some code comments assume.

### THE FIX (physical, one switch)

Set **SW4-3 to OFF** (Octo-SPI Active). Required companion: SW4-4 must
be **OFF** too (SW4-3 OFF + SW4-4 ON is an invalid combination). All
other SW4 positions are don't-care for OSPI. After flipping SW4-3 OFF,
re-run `flash_journal`; `g_fj_jedec_id` should read the real device ID
(ISSI 0x9D... per UM, or 0xC2... if the board carries the Macronix
variant) instead of `0x00FFFFFF`. Re-reading expander reg 0x0F should
then show bit 2 = 1 (SW4-3 OFF).

The CDT left-justification driver fix from earlier today stays -- it is
a genuine, FSP-confirmed correctness bug -- but it cannot be *validated*
on hardware until SW4-3 is OFF and the flash is on the bus.

## 2026-06-10 Flash POWER audit: rail is fixed/always-on, no SW-reachable enable (#44)

Premise: the user confirmed **all SW4 switches are physically OFF** (so
SW4-3 = OFF = Octo-SPI Active -- the flash *should* be on the bus), yet
the part is still electrically silent in every mode. The 2026-06-09
"SW4-3 is ON" conclusion was inferred from expander reg `0x0F = 0xF8`,
which is **known-unreliable** for switch state and is hereby retracted as
the explanation. With the flash on the bus and still dead, the remaining
software-reachable angle is whether U3's **power** (or a level-shifter
enable) is gated by something firmware can toggle. This session audited
that against the authoritative EK-RA8D2 v1 UM (R20UT5523EG0101 Rev 1.01,
Oct.20.25) and the regulator datasheet. Result: **no software-reachable
power/enable control exists.**

### Power topology (UM section 5.1, p 18)

The board has exactly **two LDOs in series**, both fixed linear
regulators with no software gate:

- **ISL80103IRAJZ**: 5 V -> 3.3 V (powers the RA MCU and most logic).
- **ISL9005AIRCZ-T**: 3.3 V -> **1.8 V** (the rail the 1.8 V IS25LX512M
  flash runs on). 300 mA current limit.

The ISL9005 (8-Ld DFN) *does* have an active-high `EN` pin (pin 2, VIH
1.4 V, datasheet FN6315). But on this board `EN` is **not** on the MCU
GPIO map and **not** on the U15 expander -- it is a power-rail housekeeping
net tied on for always-on operation. UM section 6.3 (p 35) states the
flash "is enabled for XIP mode directly **after it is powered on**" --
i.e. it self-enables at power-up with **no firmware sequence**. The
Octo-SPI Flash Assignment table (Table 29) lists only signal pins
(`OSPI_FLASH_RESET_L`=P106, `ERR_L`=P105, `C`=P808, `S_L`=P104, `DQS`=P801,
`DQ0..DQ7`) -- there is **no power-enable, no load-switch, no shifter-OE**
pin anywhere in the flash's pin assignment.

### No second I2C device, no PMIC, no I2C load switch

The UM's only I/O-expander / config-switch device is **U15 PI4IOE5V6408 @
0x43** on the system I2C bus (RIIC1, P512/SCL1, P511/SDA1). The system I2C
bus otherwise reaches only **unpopulated** ecosystem connectors (Grove J27,
Qwiic J30, Pmod, mikroBUS, Arduino). The DA7212 audio CODEC (U14) is on a
separate control path (P405/P406), not the OSPI power. The Ethernet PHY
(U11) is RGMII/MDIO, not I2C. **There is no PMIC and no I2C-controlled
load switch** that could gate U3's 1.8 V rail. A live `ra8_i2c_scan` sweep
of 0x08..0x77 on RIIC1 would therefore be expected to ACK at **0x43 only**
(plus whatever a user has plugged into the Qwiic/Grove headers) -- there is
no other on-board addressable device to find. (The live sweep could not be
executed this session: the J-Link/Pi `star@star.local` is not reachable
from the agent sandbox; the inventory above is from the authoritative UM
device list, which is exhaustive for on-board parts.)

### The flash power/enable nets, exhaustively (none SW-reachable)

| Candidate enable/power control | Where it lives | SW-reachable? |
| --- | --- | --- |
| 1.8 V rail (ISL9005 `EN`) | power-rail net, tied always-on | No (not on GPIO/expander) |
| 3.3 V rail (ISL80103 `EN`) | power-rail net, tied always-on | No |
| Flash power-enable GPIO | does not exist (not in Table 29) | N/A |
| OSPI level-shifter OE | **no level shifter** (MCU OM_0 is 1.8 V, drives the 1.8 V flash directly) | N/A |
| U15 expander bits | already swept all 256 outputs + Hi-Z + per-bit | No effect (proven) |
| SW4-3 Octo-SPI Select | **analog *bus* isolation** (DQ/CK/CS), not a power gate; mechanical DIP only | Physical only |

### Conclusion -- flash silence is a genuine hardware-layer fault

With SW4-3 OFF (flash on the bus, per the user) the IS25LX512M is still
electrically silent. There is **no software-reachable power or enable
control** for U3 -- the 1.8 V rail is a fixed always-on LDO, there is no
PMIC / load switch / level-shifter OE, and the only I2C expander (U15)
provably does not gate the OSPI path. The "nothing driven on any line"
signature is therefore either (a) the part is not actually receiving its
1.8 V VCC despite the rail being nominally always-on (a board/solder/rail
fault), or (b) the SW4-3 analog bus switch is not passing the DQ/CK/CS
nets even with the DIP OFF (a switch/routing fault). Both are hardware,
not firmware.

**The single physical measurement that resolves it:** with the board
powered, put a DMM on U3's VCC pin (the IS25LX512M VCC ball, fed from the
ISL9005 1.8 V rail -- probe the 1.8 V test point at J32-21/J32-22 too) and
confirm **1.8 V** is present at the chip. If 1.8 V is absent at U3 -> rail
/ solder fault (replace/rework). If 1.8 V is present at U3 but DQ0/CS still
sit at the pull-ups during a clocked RDID -> the SW4-3 analog switch (or
its routing) is not connecting the bus; scope SCLK/CS/DQ at the U3 pads
vs. at the MCU pads to localise the break across the switch. No further
firmware change is warranted for #44.
