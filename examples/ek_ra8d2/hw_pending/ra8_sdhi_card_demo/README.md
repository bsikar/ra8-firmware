# ra8_sdhi_card_demo -- raw-HAL SDHI block smoke test (no ra8_io / no ra8_fs)

Raw 512-byte block round-trip through the **native SDHI 4-bit host controller**
on the EK-RA8D2 (#123).  This app intentionally links neither `ra8_io` nor
`ra8_fs`; it targets the `ra8_sdcard` / `ra8_sdhi` HAL layer directly to exercise
that layer in isolation.

## What it does

1. CGC + SysTick + SCI8 console bring-up.
2. Routes the eight SDHI bus pins (port 4, pins 0..7: CMD / CLK / DAT0..3
   / WP / CD) to `PSEL = k_ra8_psel_sdhi`.
3. `ra8_sdcard_init({.instance = 0, .bus_width = k_ra8_sdhi_bus_width_4bit})` --
   runs the full SD Physical Layer identification
   (CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7), negotiates the
   4-bit data bus with CMD55 + ACMD6 (best-effort: a card that declines stays
   1-bit and init still succeeds), then steps the clock from the 400 kHz ident
   rate to the default transfer rate -- all inside `ra8_sdcard` / `ra8_sdhi`.
   No file-system layer is involved.
4. `ra8_sdcard_get_capacity` -- confirm the card reports a non-zero block count
   that covers the test block.
5. Fill a 512-byte buffer with a deterministic LCG pattern (Numerical Recipes
   coefficients, seed `0xC0FFEE11`).
6. `ra8_sdcard_write_blocks(lba=64, payload, 1)` -- write one raw block.
7. `ra8_sdcard_read_blocks(lba=64, readback, 1)` -- read it back.
8. Byte-compare the read-back against the written payload.

On a clean round-trip the app prints exactly:

```
ra8_sdhi_card_demo: native SDHI block round-trip PASS
```

Any failing step prints `ra8_sdhi_card_demo: FAIL` and parks the CPU in WFI.

**WARNING:** this app overwrites one raw block on the card (LBA 64, above the
FAT BPB at LBA 0) -- a FAT-formatted card is sufficient.

## Why this is distinct from ra8_io_sdhi_demo

`ra8_io_sdhi_demo` (and its predecessor `sdhi_card_demo`, now deleted) layer
`ra8_io` / `ra8_fs` / FAT on top of SDHI to prove the full VFS stack.  This app
stops at the `ra8_sdcard` block interface to confirm the SDHI HAL in isolation:

- No `ra8_io` blockdev vtable.
- No `ra8_fs` mount / open / read / write.
- No FAT parsing.

That distinct regression value is why both apps exist.

## board_sim gate

Under `tools/board_sim`, the native SDHI host-controller model
(`board_periph_sdhi.c`) serves a blank card attached with `--sd-new 64:fat16`.
The gate command is:

```
bash scripts/sim/smoke.sh ra8_sdhi_card_demo
```

Expected output: `ra8_sdhi_card_demo: native SDHI block round-trip PASS`.  The
emulator's end-of-run SDHI section also reports the negotiated width, e.g.
`SDHI card : 1 block reads  1 block writes  4-bit bus`, confirming the ACMD6
4-bit negotiation drove the host `SD_OPTION.WIDTH` bits.

## Why this is in hw_pending

The bench run on real silicon is yours to perform.  Once a real microSD is
inserted in the on-board slot and the PASS banner is printed by the board, move
the app to `hw_validated/hil/` with `git mv`.

## On-silicon bench plan

1. Insert any FAT-formatted microSD in the on-board slot.
2. `make ra8_sdhi_card_demo` from the repo root, then:
   ```
   make -C examples/ek_ra8d2/hw_pending/ra8_sdhi_card_demo flash
   ```
3. Open a serial terminal at 115200 baud on the J-Link OB CDC port.
4. Confirm `ra8_sdhi_card_demo: native SDHI block round-trip PASS`.
5. Move to `hw_validated/hil/`.

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 47 "SDHI")

SD command sequencing and block I/O live in `ra8_sdcard` / `ra8_sdhi`.  The
register-level citations are in those library files, not in this app.

Pin map: port 4, pins 0..7 to `PSEL = k_ra8_psel_sdhi` (RA8D2 datasheet
R01DS0493EJ, "Pin Functions", "SDHI").
