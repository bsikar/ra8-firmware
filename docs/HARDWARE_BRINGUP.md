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
| threadx_ipc_demo | ⚠️ early sample mis-flagged as ra_hw_err | retracted — see "threadx_ipc_demo ra_hw_err retraction" below; later sweeps (lines 122, 164) confirm it boots cleanly |
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

## 2026-05-02 final sweep (after ra_rand_stub fix)

The xorshift32 rand() override (commit 6d2ebbfac) was a **massive
unblocker**. Apps that previously hit ra_sbrk_trap fatal_error from
inside newlib's rand()/srand() now boot cleanly:

### ✅ Confirmed running on silicon (18 of 27 apps)
- blink, blink_hal, clock_check, uart_hello
- threadx_blink, threadx_filex_demo, threadx_canfd_demo, threadx_ota_demo
- threadx_ipc_demo (M85 sends ping; M33 has no firmware so no reply — expected)
- threadx_lwip_tcp_echo, threadx_netx_tcp_echo *(newly unblocked)*
- ethernet_tcp_echo *(newly unblocked)*
- usb_cdc_echo, usb_hid_device, usb_msc_device *(newly unblocked)*
- usb_host_cdc_echo, usb_host_keyboard, usb_host_msc_browse *(newly unblocked)*

### 🐛 Still failing (9 apps)
- **5 BLE apps** (ble_peripheral, threadx_nimble_peripheral, threadx_ble_central,
  threadx_ble_mesh_node) — radio init panics. **Renesas BLE patch image required**
  (vendor blob, documented in VENDOR_BLOBS.md). Cannot fix without the blob.
- **2 LevelX/XSPI apps** (threadx_levelx_demo, threadx_filex_levelx_demo) — both
  panic in lx_nor_flash_format. **XSPI NOR driver bring-up incomplete** for the
  IS25LX512M-JHLE chip on EK-RA8D2 v1.
- **threadx_https_client** -- stuck in main init line 239 (see
  "threadx_https_client RSIP BIST root cause" section below). Panic is in
  `demo_setup_or_halt()`, triggered by `ra_rsip_init({.run_bist=true})`,
  not Ethernet.
- **threadx_mpu_partition_demo** — ra_error_handler spin. Probably the
  intentional cross-region fault firing as designed (the fault IS the demo).
- **ra_bootloader** — stuck in ra_log.c:126. Log-loop blocking on UART; likely
  the boot stub's intentional "halt waiting for banked image" path.

## Summary

**Real bugs caught + fixed via hardware bring-up this session:**

| Commit | Bug | Impact |
|---|---|---|
| d3a9a278f | SysTick weak-alias dropped ra_time.c handler | All 36 vector tables — without fix every blink/delay app spins forever |
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
with the J-Link probe (SN 1086567198).

| App | Outcome | Evidence |
|---|---|---|
| blink (regression baseline) | **PASS** | PC=`ra_time.c:94` (ra_delay_ms loop) |
| threadx_mpu_partition_demo | **PASS — fix verified** | PC=`tx_timer_interrupt.S:237`, LR=`main.c:163` (worker thread). Pre-fix: HardFault |
| threadx_filex_levelx_demo | **FAIL — code fix insufficient** | PC=`main.c:141` (`demo_panic_halt`), LR=`main.c:244` — `lx_nor_flash_format` still returns non-LX_SUCCESS. Pin routing + DATASIZE fixes built clean but did not make the IS25LX512M actually format. Likely still missing: octal-DDR mode-switch, or RDID returning wrong manufacturer ID, or RESET timing |
| usb_hid_device | **FAIL — does not enumerate** | Chip alive in `tx_thread_schedule.S:264`, but VID 0x1209 absent from macOS `ioreg -p IOUSB`. USBX library is wired but no host-visible device |

### USB enumeration root cause (now confirmed by hardware)

`port/usbx/ux_dcd_ra_usb.c::ux_dcd_ra_usb_initialize` calls
`ra_usb_attach_handler(internal_event_cb, nullptr)` — so the DCD
bridge has a callback waiting for events. But that callback only
fires when something invokes `ra_usb_dispatch(speed)`. Grep across
the entire repo confirms **no source file** ever calls
`ra_usb_dispatch` from an IRQ context (it is linked into every
binary because `ra_usb.c` defines it, but it has zero callers).

Result: the USB controller's `INTSTS0` accumulates SETUP/CTRT bits
that nobody reads, so SETUP packets time out at the host before
the DCD ever sees them. macOS gives up enumerating.

**Fix needed** (separate task): in `ux_dcd_ra_usb_initialize`, call
`ra_isr_register(k_ra_event_usbfs_int, slot, internal_usb_isr,
prio, ctx)` (and likewise USBHS for HS-mode), where
`internal_usb_isr` is a thin C function that calls
`ra_usb_dispatch(speed)`. The ELC event codes for `usbfs_int` /
`usbhs_int` need to be added to `ra8d2_elc_regs.h::ra_elc_event_t`.

## 2026-05-02 second-round hardware verification

After USBX IRQ wiring (commit 5b65e45b9, since reverted) and xSPI
octal bring-up extensions (commit 52e373507):

| App | Outcome | Evidence |
|---|---|---|
| usb_hid_device with IRQ wiring | FAIL HardFault | PC=0xEFFFFFFE, MMFAR/BFAR=0x40700004 (out of any peripheral range). Faults the moment ra_isr_register(USBFS_INT) is called. The ELC event code 0x09A taken from FSP for older RA series is almost certainly wrong for RA8D2 |
| usb_hid_device polled (post-revert in commit a67acbc26) | PASS chip-alive, FAIL enumeration | PC=tx_thread_schedule.S:268. SYSCFG=0x411 (USBE+DPRPU+SCKE), INTSTS0=0x9F00 ticking, but VID 0x1209 still absent from macOS ioreg. Polling cadence (~30ms via jiggle period) is too slow for SETUP-window timeouts |
| threadx_filex_levelx_demo with all xSPI fixes | FAIL same panic site | PC=main.c:141 (panic_halt), LR=main.c:244, i.e. lx_nor_flash_format still returns non-LX_SUCCESS even after CMDCMP poll budget bump (64 -> 1M), tPUW reset wait (1ms -> 15ms), BMCTL0 disable, and RDID validation. UART won't drain the failure log (1-3 bytes captured at any baud) so the actual RDID response is not yet observable |

### Confirmed via JLink memory read (commit 2f2560915 + first hardware capture)

`g_ra_xspi_rdid_observed` at `0x2200448C` reads `{0x44494452, 1, 0, 0x00FFFFFF}`:
- magic = 'RDID' (probe ran)
- call_count = 1 (priv_bus_init_once executed once)
- rid_err = 0 (`ra_xspi_flash_read_id` returned `k_ra_ok`)
- jedec_id = **0x00FFFFFF** — expected `0x009D5A1A` for IS25LX512M

The all-ones response is the diagnostic floor: **the chip is not responding at all**. The xSPI controller successfully clocks out the RDID opcode and the response window completes without timeout, but the data lines come back floating high. Either:
- Chip is still in reset (RESET_L not actually driven high for tPUW)
- One of the 12 OCTA pins (CS / CK / DQS / DQ0..DQ7) is mis-routed in PSEL → chip doesn't see CS asserted, so it never drives DQ
- Chip is in a different protocol mode than the controller (chip ships in 1S-1S-1S; if a prior boot put it in 8D-8D-8D and we don't switch back via SRESET, the chip ignores 1S commands)

### Open WIP

1. USBFS_INT ELC event code for RA8D2 — verify the actual code from the RA8D2-specific FSP bsp_elc.h (currently 0x09A is suspected wrong).
2. IS25LX512M RDID actual response - UART would not drain in time to
   print the `[LX_XSPI] RDID returned 0xNNN` log line before
   `demo_panic_halt`. Diagnostic now stashed in SRAM via
   `g_ra_xspi_rdid_observed` (defined in
   `port/levelx/lx_nor_driver_ra_xspi.c`). Read it with JLink after
   the panic halt:

   1. Look up the global's address for the app under test:

          arm-none-eabi-nm examples/threadx_filex_levelx_demo/build/threadx_filex_levelx_demo.elf | grep g_ra_xspi_rdid_observed
          arm-none-eabi-nm examples/threadx_levelx_demo/build/threadx_levelx_demo.elf | grep g_ra_xspi_rdid_observed

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
        - `[2] rid_err`    = `ra_err_t` from `ra_xspi_flash_read_id`
                             cast to uint32 (`0` = `k_ra_ok`).
        - `[3] jedec_id`   = packed `(mfr<<16)|(type<<8)|capacity`;
                             expected `0x009D5A1A` for IS25LX512M-JHLE.

      Replace the address with the value from `nm` for the app you
      flashed.
3. USB polled-mode tick rate — even if the IRQ event code is wrong, a thread-pumped ra_usb_dispatch at >= 1 kHz should keep up with FS enumeration.

## 2026-05-02 MPU demo HardFault root cause + fix

`threadx_mpu_partition_demo` was previously documented as "intentional
fault catch". Wrong — there is no deliberate cross-region access in
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

## 2026-05-02 ra_bootloader spin is intentional

`examples/ra_bootloader/main.c:278` is `while(1) { wfi; }` reached when
both bank A and bank B fail `internal_bank_is_valid` — i.e. neither
slot holds an app. In the test environment we never flash apps into
bank A/B, so the spin is the design-intended terminal state. The
earlier "stuck at ra_log.c:126" observation was just a sample during
the log burst before the spin started. No bug, no fix needed.

## 2026-05-02 USB device enumeration root cause

Investigated why macOS never enumerates `usb_hid_device` despite SYSCFG showing
USBE=1, DPRPU=1, SCKE=1 and INTSTS0 ticking on the bus.

**Finding:** `ra_usb_dispatch` (the function that reads INTSTS0 and forwards
SETUP/BRDY/CTRT events to a registered handler) is **never called by anything**:
- No `ra_isr_register` for USBFS_INT in `libs/ra_hal/src/ra_usb*.c`
- No example main loop polls `ra_usb_dispatch`
- `ra_usb_phid_handle_setup` exists but has no caller — only class SETUP is
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

Investigated why `threadx_https_client` halts at `main.c:239` with **zero UART
output** (`/tmp/ra8d2-hw-test/runs/threadx_https_client.uart` is 0 bytes).

### Evidence chain

1. `arm-none-eabi-addr2line` confirms PC=0x02000368 maps to `demo_panic_halt`
   at `examples/threadx_https_client/main.c:239` -- the `wfi` inside the
   panic-spin, not the actual fault site.
2. `demo_panic_halt()` is called from 7 sites: 6 inside `demo_setup_or_halt()`
   (lines 258, 261, 264, 267, 270, 278) and one at line 925 (post
   `tx_kernel_enter` return). The boot banner `[https] booting ThreadX...`
   is printed at line 919, **after** `demo_setup_or_halt()` returns.
3. **No banner ever appears on the UART** -> the panic happened inside
   `demo_setup_or_halt()`, before line 919.
4. The only call in `demo_setup_or_halt()` that is **unique to this app**
   (i.e. not also exercised by the apps that DO boot) is
   `ra_rsip_init({.run_bist=true})` at line 277. Apps `threadx_lwip_tcp_echo`,
   `threadx_netx_tcp_echo`, and `ethernet_tcp_echo` share lines 257-270
   (cgc/time/uart/ethernet init) and they all reach their banner.

Conclusion: the panicking call is line 278, fired by
`ra_rsip_init` returning non-`k_ra_ok`.

### Why ra_rsip_init fails on real silicon

`libs/ra_hal/src/ra_rsip.c::internal_run_bist()` does:

```c
*ctrl   |= k_ra_rsip_mask_ctrl_bist;     /* arm BIST */
*status |= k_ra_rsip_mask_status_bistok; /* host-sim hack */
return internal_wait_bit(k_ra_rsip_off_status,
                         k_ra_rsip_mask_status_bistok); /* spin */
```

The OR-write into `STATUS` is described in the source comment as a
"no-op on silicon" host-sim hack so the unit test deterministically
terminates the spin without modelling the BIST sequencer. On the
RA8D2, the `STATUS.BIST_OK` bit is set by the access-management
circuit (AMC) inside the sealed RSIP-E engine, **not** by the host
write. If the RSIP-E AMC firmware is not actually running -- which is
the default state until the FSP-equivalent SCE init sequence has
loaded the AMC code page -- the bit never sets, `internal_wait_bit`
exhausts its 4096-iteration budget, and `internal_run_bist` returns
`k_ra_err_hw_init_failed`. `ra_rsip_init` propagates the error and
`demo_setup_or_halt()` jumps straight to `demo_panic_halt()`.

The hand-rolled CTRL/STATUS register layout in
`libs/ra_hal/inc/ra8d2_rsip_regs.h` (CTRL @ +0x0, STATUS @ +0x4,
ENABLE/RESET/BIST bit assignments) is **not documented in the RA8D2
Hardware User's Manual**. RSIP-E (a.k.a. SCE9) is a sealed-engine
peripheral whose only Renesas-supported entry point is the FSP
`r_sce_*` driver family. The current `ra_rsip` register definitions
were inferred for the host-sim test path and have never been
validated against silicon.

### Fix scope

Bringing real RSIP/SCE up requires:

1. Importing the FSP `r_sce` AMC firmware blob into the tree (a
   binary lifecycle / key-injection page that ships only via FSP).
2. Either translating `r_sce_subprc_select` + the SCE9 bring-up
   handshake into our HAL, or wrapping the FSP driver as a
   third-party blob behind a thin `ra_rsip_*` shim.
3. Re-validating CTRL/STATUS/CMD/MAILBOX register offsets against
   FSP's `bsp_sec.h` (the canonical layout) instead of the inferred
   host-sim layout.

This is a multi-day port comparable in scope to the USB enumeration
work (~2000 LOC of vendor driver state machine plus a binary blob
lifecycle). **Out of scope for the bring-up sweep; tracked here as
the canonical root cause for `threadx_https_client`'s line-239 halt.**

### Workaround for unblocking the HTTPS demo separately

For functional testing of the NetX-Duo + Mbed TLS data path without
touching RSIP, the demo could be patched to:

- Replace `ra_rsip_init({.run_bist=true})` with a no-op on hardware
  builds (the call is currently the only one at boot), and
- Replace the `ra_rsip_trng_read`-backed
  `mbedtls_psa_external_get_random` with the existing xorshift32
  `rand()` (already in tree per commit 6d2ebbfac) until SCE comes up.

That would prove the network/TLS stack independently of crypto
acceleration, but produces cryptographically weak entropy and is
deliberately NOT being committed -- it would mask the real bug. Doc
only.

## 2026-05-02 threadx_ipc_demo ra_hw_err retraction

The "broader silicon sweep" table above (line 106) flagged
`threadx_ipc_demo` as "ra_hw_err fired -- needs investigation". After
re-tracing the demo's code paths and re-reading the two later sweep
results in this file, that initial observation was a sampling artifact,
not a real bug. The demo runs cleanly and the entry has been amended.

### Code-path audit

`examples/threadx_ipc_demo/main.c` has exactly one halt path:
`ipc_demo_panic_halt()` (line 154), reachable only from
`ipc_demo_setup_or_halt()` and from `tx_thread_create` failure inside
`tx_application_define`. Every site that can call it is local to the
M85 boot path:

- `ra_cgc_init` -- shared with every booting app, would fail uniformly.
- `ra_cgc_get_clock_hz(cpuclk0|pclka)` -- same.
- `ra_time_init(cpuclk0_hz)` -- same.
- `ipc_demo_pins_init()` (PD_02 / PD_03 -> SCI8 async) -- same pin
  routing the verified-working `uart_hello` uses.
- `ra_sci_init(8, &sci_cfg)` -- same SCI channel as the working UART
  apps.
- `ra_ipc_channel_for_send(cpu0, 0, ...)` and
  `ra_ipc_channel_for_recv(cpu0, 0, ...)` -- pure computation that
  validates `core <= cpu1` and `pair <= 1`; both arguments are compile-
  time constants in range, so these cannot fail.
- `ra_ipc_init(&send_cfg)` / `ra_ipc_init(&recv_cfg)` -- writes
  `CLR.RST` and the IRQ/RERR/FERR clear mask to the channel's `CLR`
  register, then stores per-channel state. IPC has no `MSTPCR` gate
  (HUM Ch 3 -- always-on CPU-bus peripheral), and channels 0 and 2 are
  always reachable on the M85 side regardless of M33 state. With both
  `cfg.channel == 0` and `cfg.channel == 2` (in range), the function
  cannot return anything but `k_ra_ok`.

### Steady-state behaviour without an M33 image

Inside the worker thread (`ipc_demo_thread_entry`):

- `ra_ipc_send_message_retry(channel=2, payload=ping, retries=16)`
  writes `TXD` for the M85->M33 FIFO. The 4-stage hardware FIFO (HUM
  Ch 3.1 p 204) accepts up to 4 unread words and then reports
  `STA.FULL`; the retry helper returns `k_ra_err_hw_timeout` (NOT a
  panic) and the demo logs `[ipc_demo] send err\r\n`.
- `ra_ipc_recv_message(channel=0, &word)` returns `k_ra_err_no_data`
  whenever `STA.RDY == 0`; the loop falls through and logs
  `[ipc_demo] <no reply>\r\n`.

Neither path calls `ra_error_handler`, `ra_panic`, or any halting
helper. The demo is structurally tolerant of "M33 firmware not
loaded": it prints a pong-or-no-reply line every second forever and
never asserts.

### Reconciling the two earlier observations

- "broader silicon sweep" (line 106) sampled the chip very shortly
  after `JLinkExe loadfile` + `g; sleep ?; halt` and reported a
  transient state. Most plausibly the sample landed during the
  CGC/PLL settle window (PLL1 lock + cache enable) where the CPU is
  briefly executing inside `ra_cgc_init` -- the gdb backtrace at that
  PC does not look like a clean main-loop sample to the table-builder
  script and got bucketed as `ra_hw_err`.
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
