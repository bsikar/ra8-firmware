# ADR-0009: e-reader input, wake and service-access interfaces

* **Status** -- Proposed
* **Date** -- 2026-09-17
* **Driver** -- issue #832 (parent epic #821); consumed by #823, #824, #825, #834

## Context

Issue #832 fixes the product's user-input surface: five exposed buttons (power /
wake / shutdown / recovery, previous page, next page, volume down, volume up),
an ADXL367 accelerometer, optional ambient-light and hall sensors, and the
service access needed to recover a blank or corrupted board. None of that can
be drawn, placed or measured in this repository. What *can* be settled now,
before a schematic exists, is the contract the firmware already in this tree
imposes on that schematic, plus an honest list of what is still missing.

This ADR records that contract and that inventory. It selects no component, no
value and no timing, and it contains no measurement.

### What the firmware tree assumes today

Read against `dev` at 1657296df396871b1727a3ed10b3a9a15680bfd9.

| Board surface | Provisional pin | How the firmware drives it |
|---------------|-----------------|----------------------------|
| SW1 | P009 (port 0, pin 9) | input with internal pull-up, active-low; ICU IRQ13, falling edge, digital filter clocked from PCLKB; ELC event `icu_irq13` |
| SW2 | P008 (port 0, pin 8) | same, on ICU IRQ12 / ELC event `icu_irq12` |
| LED1 / LED2 / LED3 | P600 / P303 / PA07 | GPIO outputs, active-high, initialised low |
| Debug console | PD02 / PD03 on SCI8, optional PD04 / PD05 flow control | `ra8_board_uart_console_init` computes BRR from the live PCLKA and refuses to come up below a 16 MHz PCLKA floor |

Every one of those assignments carries a `TODO(EK-RA8P1 UM / ra8p1_kicad)`
marker: they are mirrored from the pin-compatible EK-RA8D2 because no RA8P1
board is defined yet. They are what the code assumes, not what a schematic has
decided.

Eight findings follow from reading that code against #832:

1. The board layer exposes **two** switches; the product requires **five**.
   `ra8_board_sw_id_t` is a contiguous enum with a per-switch ICU channel enum
   beside it, so five buttons mean five ids, five pins and five ICU channels.
   There is no shared-pin or dual-use path in the existing API.
2. `libs/ra8_widget` already carries a physical-button event
   (`k_ra8_widget_ev_button`, with a `button_id`), so the UI layer expects a
   stable id per physical control. The id numbering is an interface, not a
   cosmetic detail.
3. `libs/ra8_hal/inc/ra8_lpm.h` documents WUPEN0 as covering IRQ0..15 plus
   IWDT / PVD / RTC / USB. A button that must wake the MCU from Software
   Standby therefore has to sit on an ICU external-IRQ channel inside 0..15.
   The provisional IRQ12 / IRQ13 satisfy that; three more channels must be
   allocated under the same restriction.
4. `ra8_pwr_enter_software_standby` refuses to enter standby unless at least
   one wake source is already armed, so wake-source allocation is a boot-time
   requirement and not an optional extra.
5. Wake from Deep Software Standby returns through the **reset** state (the
   state machine documented in `ra8_lpm.h`). A page-turn button cannot use that
   depth if resume has to be instant; only the power control plausibly wants it.
6. `libs/ra8_hal/inc/ra8_vreg.h` documents the on-chip DCDC as **not**
   supporting Software Standby, Deep Software Standby modes 1/2/3, Battery
   Backup or Voltage Scaling Control. The deepest sleep mode the product can
   use is therefore coupled to the regulator-mode decision owned by #825; the
   two cannot be settled independently.
7. Debounce is not a constant. The ICU digital filter samples at PCLKB and the
   CGC tree belongs to the application, so the filter window moves with the
   clock configuration the product ships.
8. There is **no accelerometer anywhere in the tree**: a repository-wide code
   search for `ADXL367` returns no hits. Bus choice, chip-select or address
   default, and interrupt routing are all unallocated, so #832 is free to pick
   them and firmware has nothing to stay compatible with.

## Decision

**D1. One dedicated MCU pin per exposed button.** No key matrix and no ADC
resistor ladder. A matrix needs a driven row during sleep, and a ladder cannot
arm a WUPEN0 wake source at all; both fight finding 3 and the existing
one-pin-per-switch board API.

**D2. Every one of the five buttons lands on an ICU external-IRQ channel inside
0..15**, even where firmware only polls it today. Wake capability then stays a
schematic property rather than a later firmware workaround.

**D3. The board layer grows to five switch ids with a fixed numbering**:
power 0, previous page 1, next page 2, volume down 3, volume up 4. The
`button_id` carried by `k_ra8_widget_ev_button` mirrors that numbering, so the
reader UI can be written against it before any board exists.

**D4. The power control's forced-off path is hardware, outside the MCU.**
Firmware sees that control only as an input plus a wake source, and its net
joins no boot-mode strap (an explicit #832 requirement).

**D5. The accelerometer gets one bus, a strap-free identity and a wake line**:
either a dedicated chip select or a fixed address that needs no strap, plus at
least one interrupt routed to a channel inside IRQ0..15 so motion can wake the
MCU. A second interrupt line, if the chosen part has one, comes out to a test
point rather than being left unconnected.

**D6. Service access stays separate from user input**: SWD plus the existing
VCOM console (provisionally PD02 / PD03 on SCI8), usable with a blank or
corrupted image, and no user button on a boot-mode net.

**D7. Ambient-light and hall sensors stay outside the firmware contract** until
#832 records an owner decision. If either lands, it needs its own wake-capable
line under the same IRQ0..15 rule as D2.

## Consequences

* Five buttons claim five of the sixteen standby-capable IRQ channels, which
  the touch interrupt (#830), the radio link (#826), the charger and gauge
  (#825) and the accelerometer all also want. The pin allocation done under
  #823 and #824 has to treat IRQ0..15 as a scarce resource; this ADR is the
  reason it is scarce.
* `libs/ra8_board_ra8p1` grows from two switch ids to five once the board is
  defined. That is a board-layer change plus its tests, not an application
  change, because applications speak ids.
* Fixing the numbering now lets the reader UI and its tests be written against
  five physical controls before hardware arrives.
* Nothing here closes the electrical work in #832. The issue keeps its
  `needs-bench` state.

## Open questions -- document reads and bench work, not arithmetic

1. **Which five pins.** Needs the EK-RA8P1 User's Manual or the project's own
   schematic, plus the RA8P1 HUM Ch 20 pin-function table, to confirm each
   candidate offers an IRQn in 0..15 on the 289-ball part. Not read here.
2. **WUPEN0 bit positions** for the chosen channels (HUM Ch 14.2.19). The only
   source used above is the driver's own documentation; the register table was
   not read.
3. **Snooze on RA8P1.** `ra8_lpm.h` documents the RA8D2 as having no dedicated
   snooze register block. Whether the RA8P1 matches is unverified.
4. **Forced-off timing.** #832 already records that a nominal LTC2954
   calculation does not close this gate. Threshold, discharge, source-valid
   rearm and brownout behaviour are bench items; no timing figure is asserted
   here.
5. **Sleep mode versus regulator.** The DCDC exclusion in finding 6 has to be
   reconciled with the #825 rail design and confirmed against the HUM
   electrical tables. The resulting sleep current is a measurement, not a
   calculation.
6. **Exact accelerometer part.** Bus options, interrupt count and idle current
   need a datasheet read that this record does not contain. The orientation
   marker is mechanical and belongs with the enclosure work.
7. **Switch part and its bounce**, checked against the PCLKB-derived filter
   window on the bench once a clock tree is fixed.
8. **Ambient-light and hall sensors**: owner decision outstanding (D7).

## References

* Issue #832 (`ereader hw: design buttons, accelerometer, optional wake
  sensors, and service interfaces`); parent epic #821.
* `libs/ra8_board_ra8p1/inc/ra8_board_ra8p1.h` and
  `libs/ra8_board_ra8p1/src/ra8_board_ra8p1.c`.
* `libs/ra8_hal/inc/ra8_lpm.h`, `libs/ra8_hal/src/ra8_pwr.c`,
  `libs/ra8_hal/inc/ra8_vreg.h`.
* `libs/ra8_widget/inc/ra8_widget.h` (`k_ra8_widget_ev_button`).
* Sibling pre-board records: ADR-0005 (#846, SDRAM interface supply domain) and
  ADR-0007 (#831, front-light driver interface). Numbers may be reassigned by
  the merge train if those land in a different order.
