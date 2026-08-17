# ecc_monitor_demo

Enables full ECC ("with-check") SECDED on a spare SRAM bank, round-trips a
deterministic pattern through an ECC-protected buffer at the bank base, and
reports the ECC error-status register (#130). LED1 toggles while healthy and
LED2 on a round-trip fault; `g_ecc_ok`, `g_ecc_rw_ok`, `g_ecc_esr`,
`g_ecc_1bit`, `g_ecc_2bit` and `g_ecc_heartbeat` mirror the result for headless
probing. Needs no external hardware.

**Why a spare bank.** ECC with-check requires the bank's ECC codes to be
initialised first, or reading a never-written word raises a spurious 2-bit error
and takes the NMI. The zero-init pass rewrites the whole bank, so it has to
target memory the program does not use: this app's linker keeps `.data`, `.bss`
and the stack in the first 1 MiB (banks 0-1), and the upper banks are unused.
The `on_error` action stays NMI, but nothing injects an error here so it never
fires.

`tools/ra8_emulator` shadows the SRAM ECC control window but does not model ECC
itself, so the bring-up and the round-trip run off-target while the actual
error-detection path cannot. Confirming that a real 1-bit flip latches
`SRAMESR` (corrected) and that a 2-bit flip drives the NMI needs an error source
on the bench.

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 58 "SRAM")

- Per-bank ECC mode in `SRAMCRn` (Ch 58.2.x p 3527+); the ECC region size is
  `SRAMECCRGNn`; the deterministic zero-init pass lays down valid ECC
  (Ch 58.3.2).
- ECC error status reads from `SRAMESR` (Ch 58.2.12), decoded by
  `ra8_sram_get_status` into per-bank 1-bit / 2-bit masks.
