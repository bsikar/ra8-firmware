# mem_ecc_fault_demo

SRAM ECC fault injection and detection (#130). Where `ecc_monitor_demo` only
brings ECC up and reads a clean status, this deliberately provokes a memory
error and proves the hardware error path latches it -- which is what the SIL-3 /
DAL-B bar actually asks for.

It enables full SECDED ECC with zero-init on spare SRAM **bank 2**, chosen
because the linker leaves banks 2 and 3 unused, so corrupting a probe line there
is safe. It then runs the HUM Ch 58.3.4 ECC decoder self-test twice -- once with
a 1-bit correctable injection, once with a 2-bit uncorrectable one -- clearing
`SRAMESR` in between so each is independently proven, and reports the decoded
per-bank masks.

## Three things only silicon can show

An emulator cannot observe the syndrome *data* write that distinguishes a 1-bit
fault from a 2-bit one, because on-chip SRAM is host-backed memory rather than
an MMIO hook; it therefore latches both ESR slots on any self-test. So the
following are bench-only:

- **Per-slot fidelity** -- a 1-bit injection setting only the 1-bit flag and a
  2-bit injection only the 2-bit flag. The host test forges the correct slot
  under `RA8_OFF_TARGET`, which is not the same as confirming it.
- **1-bit correction** -- the corrected read returning good data.
- **2-bit NMI** -- on silicon the uncorrectable injection raises a
  non-maskable ECC interrupt, so a bench bring-up must install an NMI handler or
  mask faults around the 2-bit self-test. A headless run that reads `SRAMESR`
  synchronously never has to.

Bare EK-RA8D2; no shields or external transceivers.
