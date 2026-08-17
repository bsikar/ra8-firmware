# dtc_transfer_demo

SRAM-to-SRAM block copy through the Data Transfer Controller, verified against
the source pattern once a second -- the DTC counterpart to `dma_memcopy_demo`.
LED1 toggles on a good copy and LED2 on a mismatch; `g_dtc_match`,
`g_dtc_bytes`, `g_dtc_heartbeat` and `g_dtc_isr_count` mirror the result for
headless SWD probing. Needs no external hardware.

The DTC has no software-start register -- "the DTC is activated by an interrupt
request" (HUM Ch 18.3) -- so firing an ELC software event is the only way to
kick a transfer from software.

Two silicon-specific defects surfaced here that the emulator had masked, both
worth knowing before writing DTC code:

- **`DTCVBR_SEC`.** On a TrustZone part the secure DTC fetches its vector table
  from `DTCVBR_SEC` (+0x14); a secure write to the non-secure `DTCVBR` (+0x04)
  is silently dropped. `ra8_dtc_init` programs both.
- **Polled completion.** Enabling the DTC-complete CPU interrupt lets its ISR
  write `DTCSTS` while the transfer is still in flight and corrupt it, so the
  demo leaves that IRQ masked and polls the destination; the `IELSR` slot plus
  `DTCE` still activate the engine.

The emulator runs the transfer synchronously on the trigger write, which is
exactly why neither defect showed up there.

## Activation path (HUM R01UH1065EJ0130 Rev.1.30)

- TI mode word `MR = 0xA8080000`: MRA = block mode / 32-bit / SAR++, MRB = DAR++
  (Ch 18.2.2 p 786, Ch 18.2.3 p 787, Figure 18.4 p 799).
- One 256-word block: `CRA = 0x0000` (0 encodes 256, Ch 18.2.7 p 790) and
  `CRB = 1` (Ch 18.2.8 p 791).
- Vector table: `DTCVBR + slot*4` holds the 16-byte-aligned TI start address
  (Ch 18.3.1 p 796, Figures 18.2/18.3 p 797-798).
- Trigger: ELC software event 0 is ICU event `0x0CC` (Table 19.3 p 824), routed
  to the IELSR slot with `DTCE = 1` (Ch 14.2.10 p 524; activation per Ch 18.3
  p 796).

This demo does no explicit cache maintenance. With the D-cache enabled, either
place the buffers in a coherent region or clean and invalidate around the
transfer; `dtc_coherency_hil` is the app that proves that path.
