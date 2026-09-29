# ADR-0010: Bound the SDRAM interface supply domain (VCC, VCC2) separately from the system main rail

## Status

Proposed -- 2026-09-17. Blocks the regulator stage of #825 and the
memory subsystem of #827; opened against #846 under epic #821.

The DC arithmetic below is arithmetic over published guarantees. No
board has been built, so nothing here is a measurement, and the
decision stays `Proposed` until the bench items in
*Open questions* are closed.

## Context

The e-reader's proposed LTC3119 main rail (#825) reaches a complete
upper envelope of 3.584411411 V once the static feedback error and
the +/-75 mV rail allowance are stacked. #846 records that this
invalidates the guaranteed SDRAM read-high margin. The purpose of
this ADR is to name *which supply the ceiling actually belongs to*,
because the ceiling has so far been discussed as a property of the
main rail, and it is not: it is a property of the MCU's SDRAM
interface domains.

### Primary sources

Read directly for this ADR, not carried from the issue thread.

**ISSI IS42/45S32160F** (the selected U14 memory, `IS42S32160F-7TLI`),
ISSI-authored Rev. C, "DC RECOMMENDED OPERATING CONDITIONS,
IS42/45S32160F - 3.3V Operation", p.14:

| Symbol      | Parameter                                | Min   |
|-------------|------------------------------------------|-------|
| Vdd, Vddq   | Supply / I/O supply voltage              | 3.0 V |
| Vih         | Input high voltage                       | 2.0 V |
| Voh         | Output high voltage at Ioh = -2 mA       | 2.4 V |

The `Vddq - 0.2 V` output-high figure belongs to the *adjacent*
"IS42/45R32160F - 2.5V Operation" table on the same page and does not
apply to this selection. The S-family Voh guarantee is a flat 2.4 V.

**RA8P1 datasheet R01DS0439EJ0130 Rev.1.30** (Feb 27, 2026):

* Table 2.4, "I/O VIH, VIL except for Schmitt trigger input pins",
  p.46, under the `3.00 V or above` supply condition:
  `DQ00 to DQ19` requires `VIH = VCC x 0.7`, and
  `DQ20 to DQ31` requires `VIH = VCC2 x 0.7`.
  The same split appears for the non-SDRAM data bus
  (`D00 to D19` on VCC, `D20 to D31` on VCC2).
* Table 2.2, "Recommended operating conditions", p.45:
  `VCC, VCC_DCDC` **when SDRAM is used** is 3.00 to 3.63 V, and
  `VCC2` (standard product) **when 32bit SDRAM is used** is
  3.00 to 3.63 V. Outside SDRAM use both start at 1.62 V.
* No SDRAM input-threshold selector is documented. `PmnPFS.DSCR`
  selects output drive only, so firmware has no lever over the
  receive threshold. This confirms the finding already recorded on
  #846.

### What the ratio implies

The threshold is ratiometric and the memory's guarantee is flat, so
the read-high margin *shrinks as the interface supply rises*:

```
margin(V) = 2.4 - 0.7 * V
```

| Interface domain ceiling | Guaranteed read-high margin |
|--------------------------|-----------------------------|
| 3.00 V (Table 2.2 floor) | 300.0 mV                    |
| 3.25 V                   | 125.0 mV                    |
| 3.30 V                   |  90.0 mV                    |
| 3.35 V                   |  55.0 mV                    |
| 3.428571429 V            |    0 mV                     |
| 3.584411411 V (#825 max) | -109.088 mV                 |

So the usable window for a domain that carries SDRAM DQ bits is
`[3.00 V, 3.428571429 V)` minus a deliberate noise allowance, and the
binding ceiling is *not* the device's 3.63 V supply maximum. The
device would happily run at 3.63 V; this memory cannot be read
reliably there.

### Both domains carry the ceiling

Because Table 2.4 references `DQ00..DQ19` to VCC and `DQ20..DQ31` to
VCC2, tightening only one domain leaves the other half of a 32-bit
word unguaranteed. A 32-bit bus therefore puts *both* VCC and VCC2
inside the same 3.00..~3.35 V envelope, and Table 2.2's dedicated
"when 32bit SDRAM is used" row for VCC2 exists for exactly that
reason.

### What the firmware already assumes

Cross-checked against the dev tree at `013631d`:

* `libs/ra8_hal/src/ra8_sdramc.c` brings up this same ISSI part on a
  32-bit bus at `SDCLK = BCLK = 125 MHz`, CAS latency 3, refresh
  request every 900 cycles (7.2 us). The 32-bit width is what pulls
  VCC2 into the 3.00 V-minimum row above.
* The driver's 57-entry `s_sdram_bus_pins` map (A0..A12, BA0..BA1,
  DQ0..DQ31, CKE, SDCLK, DQM0..DQM3, WE, CAS, RAS, CS) is
  transcribed from the **EK-RA8D2 v1 User's Manual, Table 30**, i.e.
  it is a dev-kit board map on a different MCU, not the product
  RA8P1 pinout. Its DQ bits land on P1, P3, P6, P10 and P12. Nothing
  in the tree yet records, per DQ bit, which RA8P1 supply domain that
  bit belongs to, so no per-bit margin claim can be made from the
  firmware map as it stands.
* The driver raises every one of those pins to high-speed/high drive.
  That governs the MCU's *output* levels and edge rates into the
  memory; it does not move the receive threshold.
* The write direction is not the binding case. The memory's Vih is a
  flat 2.0 V, not a ratio of its supply, so raising the MCU domain
  makes writes easier while making reads harder. Only the read
  direction tightens as the rail rises, which is why the ceiling and
  not the floor is the live question.

## Decision

1. Treat **VCC and VCC2 as one supply constraint distinct from the
   system main rail**, and state the SDRAM requirement against that
   domain rather than against the battery/boost output:
   the complete envelope of any domain carrying SDRAM DQ bits, static
   error and transient allowance included, must stay within
   **3.00 V to 3.35 V**, which preserves at least **55 mV** of
   guaranteed read-high margin at the ceiling and 300 mV at the floor.
2. Record 3.428571429 V as the **zero-margin line** for this memory,
   and forbid it as an operating target at any tolerance corner.
3. Do not spend the guarantee to save a regulator. If the main rail
   needs a higher envelope for the radio switch drop or the NOR
   screen, the resolution is a *separate, tighter supply for the
   SDRAM interface domains*, not a relaxed margin. If instead a
   single rail feeds everything, then the main rail inherits the
   3.35 V ceiling in full, which is #846 resolution directions 1
   and 3.
4. Keep the regulator stage of #825 a draft until one of those two
   shapes is chosen with a realizable circuit.

This ADR records the constraint and the shape of the fix. It does not
select a regulator, a divider, or a part.

## Consequences

* The rail envelope discussion splits in two: a main-rail envelope
  driven by radio and NOR needs, and an SDRAM interface envelope
  bounded above by this memory's Voh. They no longer have to be the
  same number.
* If a dedicated supply for VCC/VCC2 is chosen, it has to come from a
  rail that is itself above it. Dropping 3.4185 V to a 3.25..3.35 V
  window leaves roughly 70..170 mV of headroom, which is marginal for
  a linear regulator at the MCU's peak current and needs a real
  dropout-versus-load-line check. A second switcher costs BOM,
  area and another noise source next to the memory bus.
* Lowering VCC/VCC2 lowers the output-high level of *every* pin in
  those domains, not only the memory bus, so the display FPC, touch
  and front-light interfaces on those ports need their own level
  checks before this is adopted.
* Per-bit domain assignment becomes a schematic deliverable: the
  product pin map must record, for each DQ bit, whether its threshold
  is referenced to VCC or VCC2.
* PWR-002/PWR-003/PWR-004, SYS-007, RST-001, RADIO-014 and
  CMS-010D/CMS-011 all need restating against the two-envelope model
  rather than one rail number. That work stays on #846.

## Open questions

Nothing below is settled by this ADR.

1. Does the product schematic tie VCC and VCC2 to the same net? If
   yes, there is one envelope and the 3.35 V ceiling applies to it
   whole.
2. Is a dedicated VCC/VCC2 regulator realizable from the chosen main
   rail at the MCU's peak current, including SDRAM switching? The
   headroom figures above say this needs a load-line answer, not an
   assumption.
3. What else lives on the VCC and VCC2 port groups on the product
   pinout, and does any of it require a 3.3 V-referenced output high?
4. What power-up and power-down ordering is required between VCC,
   VCC2 and the memory's own Vdd/Vddq, so the memory never drives a
   bus whose receiver domain is below the 3.00 V SDRAM condition?
5. **needs-bench.** The 2.4 V Voh is a guarantee at Ioh = -2 mA, not
   a measurement. The real part driving a real high-impedance RA8P1
   receiver over temperature will sit higher, and a first-article
   measurement should record how much. That measurement can *inform*
   a future revision of this ADR; it cannot be used to design below
   the published guarantee before the board exists.
6. **needs-bench.** The receive threshold is specified as 0.7 x VCC;
   real silicon switches lower. No margin claim may rest on that
   until it is characterized, and it should not be characterized on
   one sample.

## References

* Issue [#846](https://github.com/bsikar/ra8-firmware/issues/846) --
  main-rail voltage versus SDRAM guaranteed logic-high margin.
* Issues [#821](https://github.com/bsikar/ra8-firmware/issues/821)
  (epic), [#825](https://github.com/bsikar/ra8-firmware/issues/825)
  (power tree), [#827](https://github.com/bsikar/ra8-firmware/issues/827)
  (storage subsystems).
* RA8P1 datasheet R01DS0439EJ0130 Rev.1.30, Table 2.2 p.45 and
  Table 2.4 p.46.
* ISSI IS42/45S32160F / IS42/45R32160F datasheet Rev. C, DC
  recommended operating conditions, p.14.
* RA8P1 HUM R01UH1064EJ0130, PmnPFS / DSCR, pp.854-855.
* `libs/ra8_hal/src/ra8_sdramc.c` -- the driver whose bus map and
  32-bit configuration this ADR cross-checks.
