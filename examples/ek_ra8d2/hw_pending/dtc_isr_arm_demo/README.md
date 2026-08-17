# dtc_isr_arm_demo

The same 1 KB SRAM-to-SRAM block copy as `dtc_transfer_demo`, but it arms and
disarms DTC activation through the `ra8_isr_set_dtc()` HAL primitive (#579)
instead of open-coding the `IELSRn.DTCE` read-modify-write. Because the
primitive owns that write, this app never includes `ra8_icu_regs.h` at all --
the DTC-vs-CPU routing decision no longer leaks into application code.

Each cycle runs two phases and the verdict is the AND of both:

1. **Armed.** Fill the source with a deterministic pattern, zero the
   destination, program the Transfer Information block, set `DTCE`, and fire ELC
   software event 0. The destination must then equal the source.
2. **Disarmed.** Refill the destination with a sentinel, reprogram the TI, clear
   `DTCE`, and fire the same event. The destination must still be entirely the
   sentinel -- proof that clearing `DTCE` genuinely gates the transfer.

The ELC event is not a convenience: the DTC has **no software-start register**
(HUM Ch 18.3, "The DTC is activated by an interrupt request"), so an event is
the only way to kick it. LED1 toggles on success and LED2 on failure;
`g_dtc_armed_ok`, `g_dtc_disarmed_ok`, `g_dtc_heartbeat` and `g_dtc_isr_count`
mirror the result for headless J-Link probing. No external hardware required.

## Why a primitive

Both pre-existing DTC apps took the raw IELSR slot pointer and OR'd in the
`DTCE` bit by hand. `ra8_isr` owned IELSR slot allocation but exposed no
arm-DTC-on-slot primitive, so that read-modify-write was copy-pasted into every
DTC application. The primitive performs the identical write -- preserving the
`IELS` event field and the write-0-to-clear `IR` status flag -- so it drops into
both call sites unchanged while adding a disarm path they never had.

## Activation path (HUM R01UH1065EJ0130 Rev.1.30)

- TI mode word `MR = 0xA8080000`: MRA = block mode / 32-bit / SAR++, MRB = DAR++
  (Ch 18.2.2 p 786, Ch 18.2.3 p 787, Figure 18.4 p 799).
- One 256-word block: `CRA = 0x0000` (0 encodes 256, Ch 18.2.7 p 790), `CRB = 1`
  (Ch 18.2.8 p 791).
- Vector table: `DTCVBR + slot*4` holds the 16-byte-aligned TI start address
  (Ch 18.3.1 p 796, Figures 18.2/18.3 p 797-798).
- Arm / disarm sets or clears `DTCE` (Ch 14.2.10 p 524); activation per Ch 18.3
  p 796. Trigger: ELC software event 0 = ICU event `0x0CC` (Table 19.3 p 824).

## Blocked on

Nothing but a bench run. `ra8_emulator` models the DTC descriptor engine *and*
its `DTCE` gate -- the ELC event activates the controller only when a
`DTCE`-enabled IELSR slot links it -- so the armed copy runs and the disarmed
one is correctly skipped off-target. The activation path is byte-identical to
the silicon-validated `dtc_transfer_demo`.
