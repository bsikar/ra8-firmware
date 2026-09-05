# Clock engineering calculations

## CLK-001: Y1 main oscillator load network

Calculation revision 1, 2026-09-05. Applies to Y1, C35 and C36 on
[RA8P1 clocks, reset and debug](../ereader/mcu_clocks_debug.kicad_sch)
(sheet 5 of the current [complete schematic export](../exports/ereader_rev1.pdf)).
The schematic annotation carries the same stable identifier, CLK-001, and
links back to this document. Reference designators, not page numbers alone,
identify the circuit. Tracking: [issue #824](https://github.com/bsikar/ra8-firmware/issues/824).

Status: arithmetic checked; initial component selection only. PCB parasitics,
oscillation margin, frequency, drive level and startup time are NOT measured.
Python verifies the stated model and arithmetic, not physical operation.

### Inputs and authority

| Input | Value | Authority / qualification |
| --- | --- | --- |
| Y1 | Murata XRCGB24M000F3M19R0 | Exact part specification [1], pp.2-3 |
| Nominal frequency | 24 MHz | [1] |
| Specified crystal load CL | 6 pF | [1]; not the value of each external capacitor |
| Crystal ESR | 100 ohm maximum | [1] |
| Crystal drive | 300 uW maximum | [1]; 150 uW test level is not a board measurement |
| C35, C36 | TDK C1005NP01H080D050BA | [2] |
| Each capacitor | 8 pF +/-0.5 pF, NP0, 50 V | [2]; absolute tolerance, not +/-0.5 percent |
| Effective stray load | 2 pF nominal assumption | Unmeasured design assumption, not a guaranteed RA8P1 pin specification |
| Reference matching | 24 MHz, CL 6 pF, 8/8 pF, Rd 0 ohm | RA8P1 group 10, [3] Table 4.1.9, p.15 |

### Equivalent circuit and derivation

The crystal connects between OSC_EXTAL and OSC_XTAL. C35 connects EXTAL to
ground and C36 connects XTAL to ground. Looking between the crystal terminals,
these two external capacitors form a series path through ground:

```text
1/Cseries = 1/C35 + 1/C36
Cseries = C35*C36/(C35+C36)
CL_model = Cseries + Cstray_effective
```

This is a lumped first-order model. A more explicit model, following [3]
section 5.4 / Figure 5.4.1, separates each node's shunt capacitance from the
direct terminal-to-terminal capacitance:

```text
A = C35 + C_EXTAL_to_ground
B = C36 + C_XTAL_to_ground
CL_model = A*B/(A+B) + C_between_terminals
```

Pin, package and board capacitances contribute to those parasitic terms.
Do not simply add all pin-to-ground capacitances to CL, or count them both
explicitly and again in Cstray_effective. The lumped 2 pF assumption absorbs
their effective contribution at this proposed operating point. It is not the
crystal's internal shunt capacitance C0. Changing the capacitor values or PCB
can change the equivalent stray contribution.

For equal external capacitors C35 = C36 = C:

```text
CL = C*C/(C+C) + Cstray = C/2 + Cstray
C = 2*(CL - Cstray)
  = 2*(6 pF - 2 pF)
  = 8 pF

Back-substitution:
CL = (8 pF * 8 pF)/(8 pF + 8 pF) + 2 pF
   = 4 pF + 2 pF
   = 6 pF
```

This is why this particular 6 pF crystal has 8 pF external capacitors.
The Renesas 8/8 pF example corroborates an initial matching choice; it does
not measure 2 pF parasitics on this design or qualify this Murata part on it.
The simplified generic example in [3] uses 1 pF pin-to-pin capacitance and
zero board parasitics; substituting that assumption instead would give
2*(6-1) = 10 pF. Neither assumption replaces actual-board matching.

### Capacitor tolerance and sensitivity

At fixed assumed Cstray = 2 pF, each selected capacitor ranges from
8-0.5 = 7.5 pF to 8+0.5 = 8.5 pF. Series capacitance increases monotonically
with either positive capacitance, so the joint minimum and maximum are:

```text
CL_min = 7.5*7.5/(7.5+7.5) + 2 = 5.75 pF
CL_max = 8.5*8.5/(8.5+8.5) + 2 = 6.25 pF
Load deviation = +/-0.25/6 * 100 = +/-4.166667 percent
Opposite tolerance corners = 7.5*8.5/(7.5+8.5) + 2 = 5.984375 pF
```

This range includes initial capacitor tolerance only. It excludes unknown
parasitic error, temperature effects and measurement loading. It is NOT a
frequency-tolerance result: load pulling requires the crystal's motional
parameters or measured pulling curve. Do not convert +/-4.166667 percent
load error into the same percentage or ppm frequency error.

Sensitivity examples below are assumed scenarios, NOT characterized bounds:

| Effective stray (pF) | Required equal C for CL 6 pF (pF) | CL with fitted 8/8 pF (pF) |
| --- | --- | --- |
| 1.0 | 10.0 | 5.0 |
| 1.5 | 9.0 | 5.5 |
| 2.0 | 8.0 | 6.0 |
| 2.5 | 7.0 | 6.5 |
| 3.0 | 6.0 | 7.0 |

### ESR screening and hardware release conditions

The group-10 reference board has negative resistance -1050 ohm at this
example setting and an ESR recommendation of 210 ohm [3]. The arithmetic
1050/5 = 210 explains its fivefold screening margin. Y1's specified maximum
ESR of 100 ohm is below 210 ohm; reference ratio 1050/100 = 10.5.
That is a comparison with another board, NOT a measured margin for ours.
No external damping or feedback resistor is fitted in the initial network.

Before hardware release, qualify the actual MCU setting, startup wait,
frequency over supply/temperature, negative-resistance margin and excitation
power following [3] sections 5-6 and the MCU electrical requirements. The
24 MHz example uses the 8-to-24-MHz drive setting; its suitability still
needs confirmation on the finished circuit. Crystal drive must remain at
or below 300 uW. Do not derive actual drive from supply voltage or apply
the full MCU output swing across the crystal ESR. Measurement loading must
be controlled. Any load-value change requires recalculation, schematic/BOM
update and repeated matching. RTC calculations will be a separate record
once its exact crystal and MCU drive mode are selected.

### Reproducible arithmetic verification

From the repository root:

```sh
python3 ra8p1_kicad/scripts/check_clock_calculations.py
```

The [read-only checker](../scripts/check_clock_calculations.py) uses exact
rational arithmetic, verifies nominal, tolerance corners, sensitivity and
reference ESR ratios, and checks C35/C36/Y1 values against the exported BOM.
It does not modify the schematic, choose parts or claim electrical sign-off.
Expected output: `CLK-001 PASS: arithmetic and BOM inputs agree; hardware matching unverified.`

### References

1. [Murata exact XRCGB24M000F3M19R0 specification](https://pim.murata.com/asset/pim4/ceramicResonatorCrystalUnit/SPEC_XRCGB24M000F3M19R0_PDF_CERAMICRESONATORCRYSTALUNIT),
   JGC49-3003B, ratings and terminal diagram, pp.2-3.
2. [TDK exact C1005NP01H080D050BA specification](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1005NP01H080D050BA).
3. [Renesas oscillator design guide](https://www.renesas.com/en/document/apn/renesas-rx-and-ra-families-design-guide-main-clock-circuits-and-sub-clock-circuits-rev102),
   R01AN7202EJ0102 Rev.1.02, 2025-12-23, Table 4.1.9 and sections 5.1-5.4.
