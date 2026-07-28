# mem_ecc_fault_demo

SRAM ECC **fault-injection + detection** demo for the EK-RA8D2 (issue #130).

Where [`ecc_monitor_demo`](../ecc_monitor_demo/) only brings ECC up and reads a
(clean) status, this demo closes the loop the SIL-3 / DAL-B bar cares about: it
**deliberately provokes** a memory error and proves the hardware-error path
latches it.

## What it does

1. Enables full SECDED ECC (`with-check`) + zero-init on spare SRAM **bank 2**
   (the linker leaves banks 2-3 unused, so corrupting a probe line is safe).
2. Runs the HUM Ch 58.3.4 ECC **decoder self-test** (`ra8_sram_self_test`) twice:
   - a **1-bit** (correctable) injection, and
   - a **2-bit** (uncorrectable) injection,
   clearing `SRAMESR` between them so each is independently proven.
3. Reports the decoded per-bank 1-bit / 2-bit masks to globals and prints, once
   a second:

   ```
   ecc: sram2 1bit-inj=caught 2bit-inj=caught ok=Y
   ```

   (or `ecc: sram2 fault-inject ok=N` if either injection is not latched).

LED1 toggles while both injections are caught; LED2 toggles on a miss.

## Headless gate (ra8_emulator)

`tools/ra8_emulator` (`board_periph_sram.c`) models the decoder self-test: it
latches `SRAMESR` on the `bypass -> verify` `SRAMCRn` sequence, so
`ra8_sram_self_test` reports `out_caught` headlessly and the detection +
reporting + clear plumbing is proven end to end:

```
bash scripts/emu/smoke.sh mem_ecc_fault_demo
```

## Why it is `hw_pending`

ra8_emulator cannot observe the syndrome **data** write that distinguishes a 1-bit
from a 2-bit fault (on-chip SRAM is host-backed RAM, not an MMIO hook), so it
latches **both** ESR slots on any self-test. Three things are therefore
silicon-only and gate promotion out of `hw_pending`:

- **Per-slot fidelity** -- a 1-bit injection setting *only* the 1-bit flag and a
  2-bit injection *only* the 2-bit flag (the host test `tests/test_mem_ecc.c`
  proves this with `RA8_SIMULATOR_MODE`, which forges the correct slot, but it
  must be confirmed on real hardware).
- **1-bit correction** -- the corrected read returning good data.
- **2-bit NMI** -- the uncorrectable injection raises a non-maskable ECC
  interrupt on silicon (`on_error = interrupt`); the bench bring-up must install
  an NMI handler (or mask faults around the 2-bit self-test). ra8_emulator does not
  raise the NMI, so the headless run reads `SRAMESR` synchronously.

## Build / run

```
make                                         # cross-compile the .elf/.hex/.bin
bash scripts/emu/smoke.sh mem_ecc_fault_demo   # headless detection gate
make flash                                   # JLink load (on the bench)
```

Bare EK-RA8D2 only -- no shields or external transceivers.
