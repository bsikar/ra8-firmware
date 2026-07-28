# bscan_selftest

Headless **on-silicon self-test gate** for the JTAG / IEEE-1149.1 boundary-scan
TAP bookkeeping driver `ra8_bscan` (#138). No panel / SD / touch / external JTAG
fixture needed.

## Why a self-test and not a "real" boundary-scan demo

The RA8D2 boundary-scan TAP (HUM Ch 50, p 3257-3262) is driven by an external
manufacturing-test fixture over the four JTAG pins (TCK/TMS/TDI/TDO) **while RES
is held low**. Its four registers (JTIR / JTIDR / JTBPR / JTBSR) are *not*
memory-mapped -- the HUM is explicit (Ch 50.2.3, p 3259): the CPU cannot read or
write them. So the actual scan chain cannot be exercised from firmware; that
validation is inherently external-tool-driven (a BSDL file + a JTAG controller).

What firmware *can* validate is **its own contribution** to boundary scan: the
`ra8_bscan` bookkeeping object. This app runs that contract end-to-end:

1. `ra8_bscan_init()` seeds the expected device ID code.
2. `ra8_bscan_get_idcode()` returns the chip's hardwired JTIDR
   (`0x085D_A447`, HUM Ch 50.2.2 p 3258) -- the value the external fixture
   should scan out over TDO. Asserting it here is the firmware's authoritative
   cross-check against the fixture.
3. `ra8_bscan_set_instruction()` accepts the named JTIR opcodes (EXTEST /
   SAMPLE_PRELOAD / IDCODE / CLAMP / HIGHZ / BYPASS) and **rejects** reserved
   4-bit codes (HUM Ch 50.2.1 p 3258).
4. `ra8_bscan_clear_status()` accepts only the legal zero mask and reports
   BYPASS; `ra8_bscan_get_status()` reflects the bookkeeping snapshot.

Every entry point is exercised on both its **positive and negative** paths
(17 sub-checks total, including the NULL guards). The app halts on a `FAIL`
banner *before* the PASS line if any sub-check fails, so the gate is exact. It
then prints on the SCI8 J-Link OB console:

```
bscan: idcode=085DA447 checks=17 PASS
```

The driver touches no hardware, so the banner is identical on host, `ra8_emulator`,
and silicon -- an emulator/silicon equivalence check.

## Build + run

```
make bscan_selftest
scripts/hil/run_local.sh bscan_selftest      # flash + scrape the banner
```

The gate (`hil.conf`, `uart_scrape`) asserts the PASS line and fails on
`FAIL|HardFault|TIMEOUT`.

## Result (validated 2026-06-19, ra8_emulator)

```
bscan-selftest: boot
bscan: idcode=085DA447 checks=17 PASS
```

`scripts/emu/smoke.sh bscan_selftest` PASS (final PC in the `main` WFI
idle loop; all 17 checks passed). The driver's logic is shared with the
host unit tests in `tests/test_ra8_bscan.c`, so the same contract is covered
two ways: host unit tests (logic) + ra8_emulator (the firmware ELF, byte-for-byte
banner). On the bench the banner is expected identical -- the self-test reads a
HUM constant and validates pure bookkeeping; there is no peripheral model to
diverge.

## Scope note

This app validates the firmware-side `ra8_bscan` contract only. Exercising the
actual EXTEST / SAMPLE-PRELOAD scan vectors requires an external IEEE-1149.1
test fixture and the device's BSDL file; that is recorded as the documented
external-tool path for `ra8_bscan` (see the driver header `libs/ra8_hal/inc/ra8_bscan.h`).
