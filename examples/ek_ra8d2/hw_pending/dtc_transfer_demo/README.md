# dtc_transfer_demo

DTC (Data Transfer Controller) SRAM-to-SRAM 1 KB block copy + verify for
the bare EK-RA8D2 EVM -- the DTC counterpart to `dma_memcopy_demo`.

## What it does

Brings up SCI8 + the ISR / ELC / DTC blocks. Once a second it:

1. Fills a 1 KB source buffer with a deterministic pattern
   (`i ^ (i >> 8)`) and zeroes the destination.
2. Writes a 16-byte Transfer Information (TI) block describing a
   block-mode, 32-bit-wide, increment-both copy of one 256-word block,
   and points the DTC vector-table slot at it.
3. Re-arms `ICU.IELSRn.DTCE` on the slot and fires ELC software event 0
   (`ELSEGR0`), which activates the DTC. The DTC has no software-start
   register (HUM Ch 18.3: "The DTC is activated by an interrupt
   request"), so an ELC/peripheral interrupt is the only way to kick it.
4. Waits (bounded) for the block to land, verifies the destination, and
   prints `dtc: copied 1024B match=Y` on the J-Link OB CDC channel.

- LED1 toggles on every successful copy.
- LED2 toggles on a verification mismatch.
- `g_dtc_match` / `g_dtc_bytes` / `g_dtc_heartbeat` / `g_dtc_isr_count`
  mirror the result for headless J-Link probing.

No external hardware required.

## Why this is in hw_pending

`tools/board_sim` shadows the DTC control window (DTCCR / DTCVBR /
DTCST / DTCSTS) so the HAL bring-up reads back correctly, but it does
**not** model the descriptor-table transfer engine
(`tools/board_sim/src/board_periph_dmac.c`: "no example in the tree
triggers a DTC"). On the emulator the destination is never written and
the banner reports `match=N`. The app is therefore staged in
`hw_pending/` until the copy is confirmed on silicon.

## Activation path (HUM R01UH1065EJ0130 Rev.1.30)

- TI mode word `MR = 0xA8080000`: MRA = block mode / 32-bit / SAR++,
  MRB = DAR++ (HUM Ch 18.2.2 p 786, Ch 18.2.3 p 787, Figure 18.4 p 799).
- One 256-word block: `CRA = 0x0000` (0 encodes 256, HUM Ch 18.2.7
  p 790), `CRB = 1` (HUM Ch 18.2.8 p 791).
- Vector table: `DTCVBR + slot*4` holds the 16-byte-aligned TI start
  address (HUM Ch 18.3.1 p 796, Figures 18.2/18.3 p 797-798).
- Trigger: ELC software event 0 = ICU event `0x0CC`
  (HUM Table 19.3 p 824), routed to the IELSR slot with `DTCE = 1`
  (HUM Ch 14.2.10 p 524; activation per Ch 18.3 p 796).

## On-silicon bench plan

1. `make dtc_transfer_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; expect
   `dtc: copied 1024B match=Y` once a second with LED1 toggling.
3. Or probe headless over SWD: `g_dtc_match == 1`, `g_dtc_bytes == 1024`,
   `g_dtc_heartbeat` advancing, and `g_dtc_isr_count` advancing (the
   DTC-complete interrupt firing once DTCE auto-clears).
4. Cache coherency: with the D-cache enabled, confirm SAR/DAR point at a
   coherent region or add clean/invalidate around the transfer -- the
   demo mirrors `dma_memcopy_demo` and does no explicit cache
   maintenance.
5. Once the gate is green, move the app to `hw_validated/hil/` and switch
   `hil.conf` to the active uart_scrape gate.

Build / flash:

```
make dtc_transfer_demo
make -C examples/ek_ra8d2/hw_pending/dtc_transfer_demo flash
```
