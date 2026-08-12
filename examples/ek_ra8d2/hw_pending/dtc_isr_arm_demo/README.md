# dtc_isr_arm_demo

DTC (Data Transfer Controller) arm/disarm demo for the bare EK-RA8D2 EVM,
built on the `ra8_isr_set_dtc()` HAL primitive (issue #579).

It is the same 1 KB SRAM-to-SRAM block copy as `dtc_transfer_demo`, but it
arms and disarms DTC activation through the HAL primitive instead of
open-coding the `ICU.IELSRn.DTCE` read-modify-write. Because the primitive
owns that write, this app never includes `ra8_icu_regs.h` at all -- the
DTC-vs-CPU routing decision no longer leaks into application code.

## What it does

Brings up SCI8 + the ISR / ELC / DTC blocks. Once a second it runs two
phases and reports whether BOTH matched their expectation:

1. **Armed.** Fills a 1 KB source with a deterministic pattern
   (`i ^ (i >> 8)`), zeroes the destination, programs the Transfer
   Information block, calls `ra8_isr_set_dtc(slot, true)`, and fires ELC
   software event 0 (`ELSEGR0`). The DTC has no software-start register
   (HUM Ch 18.3: "The DTC is activated by an interrupt request"), so the
   ELC event is the only way to kick it. With `DTCE = 1` the DTC activates
   and copies the block; the destination must then equal the source.
2. **Disarmed.** Refills the destination with a sentinel (`0xA5A5A5A5`),
   reprograms the TI, calls `ra8_isr_set_dtc(slot, false)`, and fires the
   same event again. With `DTCE = 0` the DTC does not activate, so the
   destination must still be entirely the sentinel -- proof that clearing
   `DTCE` truly gates the transfer.

`good = armed_ok && disarmed_ok` gates the banner:

- `dtc-arm: armed+disarmed OK` on success (LED1 toggles).
- `dtc-arm: FAILED` on any failure (LED2 toggles).
- `g_dtc_armed_ok` / `g_dtc_disarmed_ok` / `g_dtc_heartbeat` /
  `g_dtc_isr_count` mirror the result for headless J-Link probing.

No external hardware required.

## Why a primitive

Both existing DTC apps (`dtc_transfer_demo`, `dtc_coherency_hil`) take the
raw IELSR slot pointer from `ra8_icu_ielsr(slot)` and OR in the `DTCE[24]`
bit by hand. `ra8_isr` owns IELSR slot allocation but exposed no
arm-DTC-on-slot primitive, so that read-modify-write was copy-pasted into
every DTC application. `ra8_isr_set_dtc(slot, enable)` performs the exact
same `IELSRn.DTCE` write (preserving the `IELS` event field and the
write-0-to-clear `IR` status flag), so it drops into both existing call
sites unchanged while adding a disarm path they never had.

## Activation path (HUM R01UH1065EJ0130 Rev.1.30)

- TI mode word `MR = 0xA8080000`: MRA = block mode / 32-bit / SAR++,
  MRB = DAR++ (HUM Ch 18.2.2 p 786, Ch 18.2.3 p 787, Figure 18.4 p 799).
- One 256-word block: `CRA = 0x0000` (0 encodes 256, HUM Ch 18.2.7 p 790),
  `CRB = 1` (HUM Ch 18.2.8 p 791).
- Vector table: `DTCVBR + slot*4` holds the 16-byte-aligned TI start
  address (HUM Ch 18.3.1 p 796, Figures 18.2/18.3 p 797-798).
- Arm / disarm: `ra8_isr_set_dtc(slot, enable)` sets or clears `DTCE`
  (HUM Ch 14.2.10 p 524); activation per Ch 18.3 p 796.
- Trigger: ELC software event 0 = ICU event `0x0CC` (HUM Table 19.3 p 824).

## Headless-emulator status

`tools/ra8_emulator` (`tools/ra8_emulator/src/periph/board_periph_dtc.c`)
models the DTC descriptor engine AND its `DTCE` gate: the ELC software
event activates the controller only when a `DTCE`-enabled IELSR slot links
the event. So the emulator runs the armed copy and skips the disarmed one,
producing the same `armed+disarmed OK` banner. Run it headless with the
`ra8_emulator` smoke harness once the app is built.

## On-silicon bench plan (not yet validated)

This app lives in `hw_pending/` because it has not been confirmed on a real
EK-RA8D2. Its DTC activation path is byte-identical to the silicon-validated
`dtc_transfer_demo` (the primitive performs the exact same `IELSRn.DTCE`
write), so promotion should be straightforward:

1. `make dtc_isr_arm_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; expect
   `dtc-arm: armed+disarmed OK` once a second with LED1 toggling.
3. Or probe headless over SWD: `g_dtc_armed_ok == 1`,
   `g_dtc_disarmed_ok == 1`, and `g_dtc_heartbeat` advancing.
4. Once the gate is green on the bench, move the app to
   `hw_validated/hil/` and keep the same `hil.conf` uart_scrape gate.

Build / flash:

```
make dtc_isr_arm_demo
make -C examples/ek_ra8d2/hw_pending/dtc_isr_arm_demo flash
```
