# EK-RA8D2 board manual -- audit placeholder

## Status (2026-05-01)

The Renesas EK-RA8D2 v1 User's Manual (Rev 1.01, Oct 2025, document
R20UT5523EG0101) IS publicly downloadable from Renesas and has been
committed under `docs/reference/ek-ra8d2-v1-users-manual.pdf`. It is
the official board User's Manual that ships with the kit and contains
the connector / pin-assignment tables this project needs.

What is NOT public:

- The full **EK-RA8D2 v1 Design Package** (schematic source files,
  Gerbers, BOM, layout) -- distributed by Renesas under an
  evaluation-kit license at the link below. We have not committed it
  to this tree because redistribution rights are unclear; pull it
  manually if a deeper electrical audit is needed.
- The Parallel Graphics Expansion Board 1 v1 User's Manual and the
  MIPI Graphics Expansion Board 1 v1 User's Manual are also separate
  Renesas documents (linked below) and have NOT yet been committed.
- The exact panel-side pin-out of the Renesas-supplied 7.0-inch
  1024x600 TFT module is documented in the Parallel Graphics
  Expansion Board manual, not in the EK-RA8D2 manual.

Everything below is grounded in the EK-RA8D2 v1 User's Manual unless
explicitly marked "UNCONFIRMED".

## Public Renesas documents (URLs)

| Document | URL | Status in this tree |
|----------|-----|---------------------|
| EK-RA8D2 v1 User's Manual (R20UT5523EG0101 Rev 1.01, Oct 2025) | https://www.renesas.com/en/document/mat/ek-ra8d2-v1-users-manual | committed: `docs/reference/ek-ra8d2-v1-users-manual.pdf` |
| EK-RA8D2 Quick Start Guide | https://www.renesas.com/en/document/qsg/ek-ra8d2-quick-start-guide | not committed |
| EK-RA8D2 v1 Design Package (schematic / Gerbers / BOM) | https://www.renesas.com/en/document/pcs/ek-ra8d2-v1-design-package | not committed (license-restricted) |
| Parallel Graphics Expansion Board 1 v1 User's Manual | https://www.renesas.com/en/document/mat/parallel-graphics-expansion-board-1-v1-users-manual | not committed |
| MIPI Graphics Expansion Board 1 v1 User's Manual | https://www.renesas.com/en/document/mat/mipi-graphics-expansion-board-1-v1-users-manual | not committed |
| Renesas EK-RA8D2 product page | https://www.renesas.com/en/design-resources/boards-kits/ek-ra8d2 | live |

## Index of pending pin-mux confirmations

This table is a manually curated closure record, not generated output: most
rows are resolved and their markers are gone, so no grep reproduces it. The
markers still outstanding in the tree are found with:

```
grep -rnE "TODO: confirm|TODO\\(board-rev\\)" examples/ libs/
```

| File:line | Subsystem | Firmware-guessed pin | Manual-confirmed pin | Status |
|-----------|-----------|----------------------|----------------------|--------|
| `examples/_unsupported/audio_loopback/src/main.c` (`internal_audio_pins`) | SSI / I2S audio CODEC (DA7212 U14) | P5_00..P5_04 | See "Audio CODEC" below; actual pins are P403/P404/P405/P406/PD06/P511/P512 (Table 32 of UM) | RESOLVED -- the firmware now uses the confirmed pins |
| `examples/_unsupported/audio_loopback/README.md` ("Pin assignments") | SSI / I2S audio CODEC | AUDIO_MCK on P5_00 etc. | MCLK = PD06, BCLK = P403, WCLK = P404, DATIN = P405, DATOUT = P406, I2C SDA = P511, SCL = P512 (UM Table 32) | RESOLVED |
| `examples/ek_ra8d2/hw_validated/manual/lcd_color_cycle/`      | GLCDC parallel TFT pin table       | unspecified J57     | Connector is **J1** (Parallel Graphics Expansion Port), pins per UM Table 33 -- see "GLCDC" below   | RESOLVED in the renamed validated app; per-pin PSEL still UNCONFIRMED for some entries |
| `examples/ek_ra8d2/hw_validated/manual/lcd_color_cycle/`      | GLCDC connector reference          | J57                 | Correct connector is **J1**, not J57                                                                | RESOLVED -- stale label removed during the validated-app rewrite |
| `examples/ek_ra8d2/hw_validated/manual/lcd_color_cycle/README.md` | GLCDC pin table                 | every entry         | Use UM Table 33 directly                                                                            | RESOLVED |
| Retired pre-migration e-reader demo; replacement: `apps/board/stand_alone/ereader/` | GLCDC parallel TFT (shared) | J57 | Same board correction -- J1 + Table 33 | Historical finding; old demo removed |
| Retired pre-migration e-reader demo; replacement: `apps/board/stand_alone/ereader/` | SDHI SD-card pin table | unspecified | **EK-RA8D2 v1 has NO microSD socket.** SDHI is not exposed on this board. | Historical finding; old demo removed |
| Retired pre-migration e-reader demo; replacement: `apps/board/stand_alone/ereader/` | GLCDC bring-up call sites | n/a | Same as above | Historical finding; old demo removed |
| Retired pre-migration e-reader demo; replacement: `apps/board/stand_alone/ereader/` | ICU buttons SW1/SW2 | SW1 -> IRQ11, SW2 -> IRQ12 | **SW1 -> P009 / IRQ13-DS, SW2 -> P008 / IRQ12-DS** (UM Table 25, Section 5.5.2) | Historical finding; old demo removed |
| `examples/_unsupported/motor_3phase/src/main.c` (`s_motor_3phase_pin_u/v/w`) | GPT0/GPT1/GPT2 GTIOCxA outputs     | P4_08 / P4_09 / P4_10 | **WRONG pins.** P408/P409/P410 are routed to the on-board J-Link debug MCU as UART pins (UM Section 5.4 / Pmod table). The EK-RA8D2 publicly exposes GTIOC1A on P105 and GTIOC2A on P103 via the Arduino Uno header (UM Table 20). GTIOC0A is not brought out on any documented header. | UNCONFIRMED -- needs design decision: either use P103/P105 (and drop GTIOC0A), or run motor_3phase off Pmod GTIOC pins (P810/P811 = GTIOC10A/B per UM Table 21) |
| `examples/_unsupported/motor_3phase/README.md` ("Pin assignments") | Same as above                      | P4_08 / P4_09 / P4_10 | Same as above                                                                                       | UNCONFIRMED |

## Audio CODEC pins (UM Table 32, Page 38) -- AUTHORITATIVE

The audio CODEC is a DA7212 (U14) wired to the RA8D2 via SSIE / I2S
plus an I2C control link. The firmware previously guessed `P5_00..P5_04`;
the actual wiring, now implemented, is:

| CODEC pin | Function       | RA8D2 signal |
|-----------|---------------|--------------|
| DATIN     | DAI data in   | P405 (shared with parallel camera; J41 jumper) |
| DATOUT    | DAI data out  | P406 (shared with parallel camera; J41 jumper) |
| BCLK      | DAI bit clock | P403 |
| WCLK      | DAI word clock| P404 |
| MCLK      | Master clock  | PD06 |
| SDA       | I2C data      | P511 |
| SCL       | I2C clock     | P512 |

Note: P405 / P406 are shared with the parallel-camera connector. J41
jumpers must be fitted to route them to the CODEC, and the camera must
not be in use simultaneously.

PSEL values (PFS PSEL bitfield) for these pins are NOT in the board
manual -- they are in the chip Hardware User's Manual
(`ra8d2-hardware-user-manual.pdf`) under "I/O Ports". Look up each pin's
SSIE/I2C alternate function and write the PSEL accordingly. Until
that walk is done, mark the PSEL value itself as
"UNCONFIRMED -- needs HW UM cross-check".

## GLCDC parallel TFT pins (UM Table 33, Page 42) -- AUTHORITATIVE

The parallel-RGB TFT plugs into **connector J1**, the "Parallel
Graphics Expansion Port". The firmware's `J57` label is wrong.
Sample rows (RGB888 mode):

| J1 pin | Function         | RA8D2 signal |
|--------|------------------|--------------|
| J1-9   | VSYNC / TCON0    | P806 |
| J1-10  | CLK              | P515 |
| J1-11  | DE / TCON2       | P807 |
| J1-12  | HSYNC / TCON1    | P805 |
| J1-13  | EXTCLK           | P710 |
| J1-14  | TCON3            | P513 |
| J1-15..J1-22 | DATA1..DATA6 (B / G low bits) | P914/P915/P902/P903/P911/P910/P913/P912 |
| J1-23..J1-30 | DATA8..DATA15 (G high / lower R) | P207/P904/PB06/PB07/PB01/PB05/PB03/PB04 |
| J1-31..J1-38 | DATA16..DATA23 (R) | PB00/PB02/P711/P707/P713/P712/P715/P714 |
| J1-2 / J1-4 | I2C SDA1/SCL1 (touch) | P511 / P512 |
| J1-1   | BLEN             | P514 |
| J1-3   | INT (touch)      | P111 |
| J1-6   | RESET_L          | P606 |

Full table including RGB666 / RGB565 mappings is in UM Table 33. PSEL
values per pin are in the chip HW UM, not the board UM.

## SD-card / SDHI -- NOT POPULATED on EK-RA8D2 v1

`grep -i 'SDHI\|microSD\|SD card'` over the EK-RA8D2 v1 User's Manual
returns zero hits. The board does not expose SDHI. The retired pre-migration
e-reader demo's SDHI bring-up therefore required an external add-on board not
described by the EK-RA8D2 v1 manual. The current
`apps/board/stand_alone/ereader/` app must keep any removable-storage path
explicitly bound to documented external hardware.

## User switches (UM Table 25, Page 31) -- AUTHORITATIVE

| Designator | RA8D2 port | IRQ                | Trace-cut jumper |
|------------|-----------|---------------------|------------------|
| SW1        | P009      | **IRQ13-DS**        | E31              |
| SW2        | P008      | IRQ12-DS            | E32              |
| SW3        | RESET_L   | (reset)             | --               |

The firmware comment `SW1 -> IRQ11` is incorrect; SW1 is IRQ13-DS.
SW2 -> IRQ12 is correct.

## Motor 3-phase (UNCONFIRMED -- design decision required)

The EK-RA8D2 v1 board does not break GTIOC0A/1A/2A onto a single
clean header trio. Documented public GTIOC pins (UM Tables 20 and 21):

| Header        | GTIOC pin                          | RA8D2 port |
|---------------|------------------------------------|-----------|
| Arduino Uno   | GTIOC1A (D13)                      | P105 |
| Arduino Uno   | GTIOC2A                            | P103 |
| Arduino Uno   | GTIOC8A / GTIOC8B / GTIOC10A/B     | P101/P100/P810/P811 |
| Pmod          | GTIOC10A                           | P810 |
| (none public) | GTIOC0A                            | -- |

The current `examples/_unsupported/motor_3phase` choice of `P4_08 / P4_09 / P4_10`
maps to debug-MCU UART pins (`P408/P409/P410`) on the EK-RA8D2 v1
board and CANNOT drive a 3-phase inverter from the user MCU. The
example needs to either:

1. Re-pin to P101 / P102 / P103 / P104 / P105 (GTIOC8A/B + GTIOC2A/B
   + GTIOC1A/B on the Arduino header) -- two GPT channels, six PWM
   legs -- and drop the GTIOC0A requirement.
2. Move to a Pmod-based 3-phase break-out and use GTIOC10A/B.
3. Be marked as not bringing-up on the EK-RA8D2 evaluation board and
   targeted at a custom PCB.

This is a design choice, not a documentation lookup, so the marker
stays UNCONFIRMED in this manual audit.

## Reference material

- Chip datasheet: `docs/reference/ra8d2-datasheet.pdf` (R01DS0493EJ).
- Chip Hardware User's Manual:
  `docs/reference/ra8d2-hardware-user-manual.pdf` (R01UH1065EJ) --
  authoritative for PFS PSEL values once a port-pin is chosen.
- Board User's Manual (this audit's primary source):
  `docs/reference/ek-ra8d2-v1-users-manual.pdf` (R20UT5523EG0101 Rev
  1.01, Oct 2025).
- Renesas FSP source tree `board/ra8d2_ek/board.c` (read-only
  external reference).

## How to re-run this audit

```
grep -rn "TODO: confirm" examples/ libs/
```

Each marker that is then resolved against the board UM should be
deleted from the source AND from the table in this file in the same
commit, so the audit list does not drift. New markers must be
appended.
