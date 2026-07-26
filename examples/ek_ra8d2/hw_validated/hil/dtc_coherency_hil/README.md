# dtc_coherency_hil

Single-core (Cortex-M85) HIL validator that boots with the **L1 I-cache +
D-cache and the 5-region MPU enabled** (`RA8_BOOT_ENABLE_CACHE_MPU`) and proves
the **DTC descriptor cache-coherency wiring** keeps a software-triggered DTC
mem-to-mem copy correct with the M85 D-cache turned ON. This is the on-silicon
arm of the T4 cache-coherency chain for the DTC.

## Status: hw_pending (passes board_sim; awaiting on-silicon HIL run)

`board_sim` models memory byte-exact and does **not** model the L1 D-cache, so
the clean/invalidate calls are exercised (line-size + barrier logic run) but have
no caching effect and the copy verifies trivially. The sim run proves the app
boots, drives the real `ra8_dtc` + ELC path, and reports PASS with the cache + MPU
boot path compiled in; the bench run proves the same on real silicon, where a
missing descriptor clean would let the DTC fetch a stale vector-table entry or TI
and corrupt the transfer.

## Why this app exists

The Data Transfer Controller fetches its **DTC vector table** -- and, one
indirection on, each per-source **Transfer Information (TI)** block -- straight
from SRAM at activation. With the M85 D-cache enabled, the CPU's writes to those
descriptors (the `s_dtc_vt[slot]` entry and the TI fields) sit **dirty in cache**
where the engine cannot see them. `ra8_dtc_enable()` was wired (commit `b80c247b`)
to `ra8_cache_dcache_clean_by_addr()` the caller-populated vector table back to RAM
before `DTCST = 1`. The DTC is direction-blind (like the DMAC), so this app -- as
the owning driver -- cleans the **TI block + source buffer** and invalidates the
**destination buffer** itself. This test validates that whole chain on silicon.

## What it does (one-shot, then WFI)

`SystemInit()` (shared boot, `libs/ra8_board_ek_ra8d2/boot/system_init.c`) runs
with `RA8_BOOT_ENABLE_CACHE_MPU` defined for this app, so it programmes the MPU and
enables the I-cache, D-cache, and branch predictor **before** `main()`. The
vector table, the TI block, and both 1 KiB data buffers live in MPU **region 1**
(`0x22000000`, M85-private **cacheable** SRAM), so the cache hazard is real.

1. `ra8_cgc_init` + `ra8_mstp_init` + `ra8_isr_init` + `ra8_elc_init` + SCI8 console + LEDs.
2. `ra8_dtc_init(s_dtc_vt)` programmes `DTCVBR`; `ra8_isr_register` allocates an
   IELSR slot for ELC software event 0 (its slot index is the DTC vector number).
3. Fill `s_src` with `s_src[i] = i ^ (i >> 8)` (dirty in cache); prime `s_dst`
   with a sentinel so a no-op copy fails the verify.
4. Write the 16-byte TI block (256-word, 32-bit, increment-both block copy) and
   point `s_dtc_vt[slot]` at it (both dirty CPU writes).
5. Clean `s_src` + `s_dtc_ti`; `ra8_dtc_enable()` (HAL cleans the vector table,
   then `DTCST = 1`); arm `IELSRn.DTCE`; fire ELC software event 0.
6. Poll for the copy by **invalidating `s_dst` and re-verifying** each iteration
   (the read therefore observes RAM, not the stale sentinel still cached) until
   `s_dst == s_src` (bounded). This poll is both the completion gate and the
   coherency proof -- a stale descriptor leaves `s_dst` wrong and times it out.
7. PASS -> `dtc_coherency_hil: dtc coherent PASS` over VCOM + ITM, LED1 toggles.
   Mismatch / timeout -> a distinct `... FAIL` line (never `PASS`) + LED2. Either
   way the core parks in WFI so `board_sim`'s idle-stop terminates the run.

The completion IRQ is intentionally left masked (the app never calls
`ra8_isr_globals_enable`): the registered slot + `DTCE` still activate the DTC, the
app simply polls for the result rather than taking the interrupt, avoiding the
completion ISR (which writes `DTCSTS`) racing the in-flight copy.

## board_sim note

`tools/ra8_emulator` DOES model the DTC transfer engine (`board_periph_dtc.c`): the
ELC software-event trigger reads the vector-table entry at `DTCVBR + slot*4`,
fetches the TI, and actually MOVES the bytes in emulated memory, so the **real**
`ra8_dtc` + ELC path runs in sim and the copy verifies. board_sim's memory is
byte-exact and it does not model the L1 D-cache, so the poll falls through on its
first iteration. The cache hazard this app guards against is only observable on
real silicon.

## Build + run

From the repo root:

```sh
make dtc_coherency_hil                 # cross-compile -> build/dtc_coherency_hil.elf / .hex / .bin
make flash-dtc_coherency_hil           # flash via on-board J-Link OB
make sim-dtc_coherency_hil             # boot the real .elf on the board_sim CPU emulator
```

Headless `board_sim` (one-shot banner, idle-stop):

```sh
BOARD_SIM_WALL_S=15 BOARD_SIM_IDLE_STOP=1 \
  tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_validated/hil/dtc_coherency_hil/build/dtc_coherency_hil.elf \
    --panel tools/ra8_emulator/panels/ek_ra8d2.toml
```

The VCOM console line `dtc_coherency_hil: dtc coherent PASS` (or an `[itm]` mirror
of it) appears in the output.

## HIL gate (`hil.conf`)

`uart_scrape` mode: the Pi rig reads `/dev/ttyACM0` at 115200 8N1 and gates on the
success banner, failing on any `FAIL|HardFault|TIMEOUT|panic`.

```
HIL_MODE=uart_scrape
HIL_EXPECT="dtc_coherency_hil: dtc coherent PASS"
HIL_EXPECT_NEGATIVE="dtc_coherency_hil: dtc coherent FAIL|FAIL|HardFault|TIMEOUT|panic"
HIL_TIMEOUT_S=15
```
