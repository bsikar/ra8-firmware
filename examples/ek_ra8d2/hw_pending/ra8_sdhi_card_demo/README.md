# ra8_sdhi_card_demo

A raw 512-byte block round-trip through the native SDHI 4-bit host controller
(#123). This app intentionally links **neither `ra8_io` nor `ra8_fs`** -- no
block-device vtable, no mount, no FAT parsing -- targeting the `ra8_sdcard` /
`ra8_sdhi` HAL layer directly so that layer is exercised in isolation. That
distinct regression value is why it exists alongside `ra8_io_sdhi_demo`, which
layers the full VFS stack on the same controller.

It routes the eight SDHI bus pins, runs the full SD Physical Layer identification
(CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7), negotiates the 4-bit
data bus with CMD55 + ACMD6, and steps the clock from the 400 kHz identification
rate to the default transfer rate. The 4-bit negotiation is **best-effort**: a
card that declines stays 1-bit and init still succeeds. It then confirms a
non-zero capacity covering the test block, writes one raw block of a
deterministic pattern, reads it back, and byte-compares. Any failing step parks
the CPU in WFI.

> **WARNING:** this app overwrites one raw block on the card, at an LBA above the
> FAT BPB. A FAT-formatted card is sufficient, but the block is gone.

Off-target the SDHI model serves a blank card image and reports the negotiated
width in its end-of-run summary, which is what confirms ACMD6 actually drove the
host `SD_OPTION.WIDTH` bits.

## Blocked on

A bench run with a real microSD in the on-board slot.

## Registers

SD command sequencing and block I/O live in `ra8_sdcard` / `ra8_sdhi`, which
carry the register-level citations for HUM R01UH1065EJ0130 Ch 47 "SDHI". Pin map:
port 4, pins 0..7 to `PSEL = k_ra8_psel_sdhi` (RA8D2 datasheet R01DS0493EJ, "Pin
Functions", "SDHI").
