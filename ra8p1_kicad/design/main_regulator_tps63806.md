# PWR-006: TPS63806 main-regulator replacement design basis

Revision 2 native migration checkpoint, 2026-09-08. Tracking: [power #825](https://github.com/bsikar/ra8-firmware/issues/825),
[architecture #823](https://github.com/bsikar/ra8-firmware/issues/823), and
[SDRAM voltage compatibility #846](https://github.com/bsikar/ra8-firmware/issues/846).

**Native regulator migration saved and connectivity-audited; not qualified.**
U13 has been replaced in the native editor with TPS63806YFFR, with the
passive and reference bindings below. The independent netlist audit checks
all 15 balls and preserves all 307 unaffected endpoint partitions. There
are 210 components and 312 nets. Native and CLI ERC report 139 errors and
two warnings, unchanged from the preceding checkpoint; these are existing
unfinished-project violations, not a clean ERC result. No ERC settings or
exclusions were added or changed. The refreshed native BOM contains 207
included references in 80 groups and 19 columns, including Description;
only TP1-TP3 are excluded. The pre-migration implementation is described by
[PWR-004](main_regulator_ltc3119.md); its voltage, external compensation and
startup calculations do not transfer to TPS63806. Reset coordination,
source protection and qualification remain separate acceptance steps.
[RST-002](reset_coordination_tps3890.md) now records the wired native U2
TPS389001DSET with R67/R74 33k/20k, R75 10k MR pull-up and C95 10n CT;
the targeted final U2 connectivity check is complete. Native/CLI ERC remains
139 errors and two warnings with no U2 violations or changed rules/exclusions.
BOM/PDF and native reset notes are refreshed; changed MCU/radio pages were
visually inspected. This is not whole-circuit validation. Radio R12/R13 now
use the same 33k/20k threshold basis, but U4 remains TPS22917. Its old
87.5 mV path-loss screen gives -48.959308 mV radio release headroom;
the proposed 20 mV replacement-path allocation is not implemented.
Joint radio DC coordination is unresolved. This does not adopt the
fast-brownout fallback or imply fabrication approval. Counts and audit
results above describe the regulator checkpoint, not the later reset edits.

### Native migration checkpoint

The project-local `Power_Devices:TPS63806YFFR` symbol is now saved in
[Power_Devices.kicad_sym](../libs/symbols/Power_Devices.kicad_sym). It has
10 pin groups representing all 15 physical balls, all visible, with 200 mil
pin lengths, endpoints on a 100 mil grid and 50 mil pin text. The native
symbol checker reports no issues. U13 has now been replaced through the
native editor, at the working sheet position 6500,4000. The connectivity
audit verifies VIN/MODE, EN, both switch nodes, VOUT, FB, all grounds and
the intentional PG no-connect. C95/C97/C98/R67 were removed; C94 is now
the third local 22 uF output capacitor. U16/Q3 control wiring is retained.
Four linked PWR-006 schematic notes expose the calculations below; embedded
Python assertions pass, with an independent Decimal arithmetic check.
The displayed 0.938689 mV Q3 clamp bound is a conservative rounded-up
bound on 0.938688177 mV. These checks do not qualify physical hardware.

The symbol's footprint is deliberately blank. Selection and verification
of the exact YFF 15-ball WCSP land pattern, ball numbering, package revision
and assembly capability are deferred. New-MPN passive footprints are also
intentionally deferred/blank rather than silently inheriting incompatible
land patterns. These are layout-release blockers, not permission to reuse
the LTC3119 QFN, former inductor or old capacitor footprints.

## Proposed regulator and divider

Primary basis: [TI TPS63805/TPS63806/TPS63807 SLVSDS9E Rev E](https://www.ti.com/lit/ds/symlink/tps63806.pdf),
Table 7-1, Sections 8.3-8.5, 9.3 and 10.2.2. Use TPS63806YFFR, the
15-ball WCSP variant. Its 0.500 V nominal feedback and PWM-mode +/-1%
accuracy underpin this calculation. MODE must be held high; the same
envelope is not approved for automatic PFM operation. The specified FB
bias maximum is 100 nA at the electrical-table conditions.

[DigiKey 296-TPS63806YFFRCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/TPS63806YFFR/10715517)
was recorded at 1,515 in stock in the 2026-09-08 sourcing checkpoint, with
USD 3.04 / 2.277 / 1.8744 at quantities 1 / 10 / 100. A later same-day
read showed 1,485 in stock with those prices unchanged. These are unreserved
snapshots, not a purchase or quote; shipping, tax and possible tariff are
excluded. These sourcing snapshots do not verify the completed circuit.

| Proposed role | Exact component | Nominal / initial tolerance / TCR |
| --- | --- | --- |
| R41, output sense to FB | Susumu RG2012V-562-P-T1 | 5600 ohm / +/-0.02% / +/-5 ppm/C; 0805 |
| R42, FB to lower-series node | Susumu RG2012L-102-L-T05 | 1000 ohm / +/-0.01% / +/-2 ppm/C; 0805 |
| R66, lower-series node to quiet ground | YAGEO RC0603FR-0710RL | 10 ohm / +/-1% / +/-200 ppm/C; 0603; repurposed former LTC RT reference |

The 10 ohm part is in series with the 1000 ohm lower leg, not parallel
with it or in series with FB. The resulting 1010 ohm nominal lower leg
is below TI's 100 kOhm maximum. No feedforward capacitor is selected.
The 0805 precision parts require intentional footprint changes from the
existing divider; an MPN-only substitution is insufficient.

Sources: [Susumu RG catalog, electrical and reliability tables](https://www.susumu.co.jp/common/pdf/n_catalog_partition01_en.pdf),
[Susumu ultra-precision RG specification and order coding](https://www.susumu.co.jp/common/pdf/RG_LL_Data_Sheet.pdf),
and [YAGEO exact 10 ohm specification](https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-0710RL).
These establish the tolerance/TCR selections. Divider and MLCC procurement
snapshots below were checked 2026-09-08; quantities are unreserved and
prices exclude shipping, tax and possible tariff.

| References | Exact part / DigiKey cut-tape code | Indexed stock | USD at 1 / 10 / 100 |
| --- | --- | ---: | --- |
| R41 | RG2012V-562-P-T1 / 408-RG2012V-562-P-T1CT-ND | 702 | 2.18 / 1.808 / 1.5074 |
| R42 | RG2012L-102-L-T05 / 408-1631-1-ND | 501 | 3.66 / 3.026 / 2.5312 |
| R66 | RC0603FR-0710RL / 311-10.0HRCT-ND | 36360 | 0.10 / 0.037 / 0.0187 |
| C74, C75, C94, C96 | CL32B226MOJNNNE / 1276-3395-1-ND | 162775 | 0.52 / 0.318 / 0.2122 |

Sources: [R41](https://www.digikey.com/en/products/detail/susumu/RG2012V-562-P-T1/1248344),
[R42](https://www.digikey.com/en/products/detail/susumu/RG2012L-102-L-T05/3737817),
[R66](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710RL/726879),
[Samsung MLCC](https://www.digikey.com/en/products/detail/samsung-electro-mechanics/CL32B226MOJNNNE/3891481).

### Installed-resistance and rail acceptance envelope

Each endpoint stacks independent signed factors multiplicatively. This is
an engineering acceptance model, not a manufacturer promise that aging,
assembly, humidity and temperature limits apply simultaneously over an
unlimited service life. The 100 C excursion is from the resistor reference
temperature; actual resistor temperatures, including self-heating, must
remain within that excursion and the applicable power derating.

| Leg | Multiplicative terms, in addition to nominal resistance | Absolute endpoint allowance |
| --- | --- | ---: |
| Upper 5600 ohm | Initial 0.02%; TCR 5 ppm/C times 100 C; aging 0.1%; assembly 0.05% | +/-0.02 ohm |
| Lower 1000 ohm | Initial 0.01%; TCR 2 ppm/C times 100 C; aging 0.1%; assembly 0.05% | +/-0.01 ohm |
| Lower 10 ohm | Initial 1%; TCR 200 ppm/C times 100 C; one combined additional aging/assembly 1% | +/-0.05 ohm |

The extra 1% for the 10 ohm part is one combined factor, not two separate
1% factors. Aging/assembly and absolute terms are explicit project
allocations; satisfying them in the assembled product remains required.
The FB calculation conservatively applies +/-100 nA times the maximum
upper resistance to both voltage endpoints. This deliberately relaxes
corner correlation on the lower endpoint. No additional board leakage
budget is hidden inside the silicon maximum: leakage exceeding the
remaining combined allocation requires a revised envelope.

```text
Vnom = 0.500*(1 + 5600/(1000+10)) = 3.272277227723 V
Rupper = 5587.669237200..5612.349242800 ohm
Rlower_1000 = 998.190969820..1001.810970180 ohm
Rlower_10 = 9.554980000..10.455020000 ohm
Vstatic_low = 0.495*(1 + Rupper_min/Rlower_max) - 100nA*Rupper_max
Vstatic_high = 0.505*(1 + Rupper_max/Rlower_min) + 100nA*Rupper_max
Vstatic = 3.226819680019..3.318012496197 V
Vcomplete = Vstatic +/- 0.075 V = 3.151819680019..3.393012496197 V
```

The 75 mV is one combined allowance for remaining regulation effects,
ripple, transients, routing/ground offsets and board leakage. It is not
75 mV for each mechanism, and is not a measured waveform or guaranteed
transient specification. Acceptance is required at the load pins and over
all permitted source/load modes. Rounded nominal labels such as "3.3 V"
must not replace the endpoints in dependent calculations.

As an arithmetic comparison against the #846 inputs, the full upper
endpoint gives `2.4 - 0.7*Vcomplete_high = 24.891253 mV`. This is positive
static logic-high headroom, not an additional noise allowance or a closure
of memory qualification. The rail is 206.987504 mV below 3.6 V at the upper
endpoint and 151.819680 mV above 3.0 V at the lower endpoint. Reset release,
radio-switch drop and every connected load's actual limits still require
their own coordinated review. RST-002's MCU reset selection is implemented
and its targeted connectivity checked; radio DC coordination and full-system
reset qualification remain unresolved. Do not carry forward PWR-004's
reset-margin conclusions.
The separately preserved [brownout fallback](main_rail_brownout_tps63806.md)
is explicitly proposed, not adopted by this migration.

## Native pin mapping and preserved held-enable control

This table matches the independently audited native connectivity.
The ball mapping is from TI Table 7-1; physical layout and the later
footprint still require verification against the manufacturer's drawing.

| TPS63806 balls | Native connection / qualification requirement |
| --- | --- |
| A2, A3 VIN | Raw SYS_AON with locally qualified input bypass |
| B2, B3 L1 | One end of the new inductor |
| D2, D3 L2 | Other end of the new inductor |
| E2, E3 VOUT | +3V3_MCU with locally qualified output bypass |
| C2, C3 GND; C1 AGND | Common ground system; quiet divider return at AGND |
| D1 FB | Junction of upper 5600 ohm and lower 1000+10 ohm divider |
| A1 EN | Converter-side node after existing R68, also connected to R69 and Q3 drain |
| B1 MODE | Raw SYS_AON for forced PWM; no floating or firmware-dependent startup state |
| E1 PG | Unused; leave unconnected, not substituted for reset supervisors |

Preserve PWR-004's raw-powered U16 74LVC1G17GW,125 and C101 bypass,
MAIN_PWR_EN input, R68 1 kOhm series output, R69 68 kOhm pulldown, and
held Q3 DMN2056U-7 clamp. Q3 gate remains driven by POWER_OFF_H through
R70 1 kOhm, with R71 1 MOhm to ground; its source remains grounded.
Retain R34 110 kOhm from AON_HOLD to MAIN_PWR_EN and the existing U12,
KILL and main-discharge topology. The former RUN node now serves EN;
its drain must not be merged with KILL, discharge or future USB-clear drains.

### Recalculated EN and held-control screens

Use TI Section 8.5 EN/MODE high >=1.2 V, low <=0.4 V and 0.2 uA maximum
input leakage at its stated VIN = 1.8..5.5 V electrical-table conditions.
Do not extend that leakage specification to zero VIN. Primary control
references are [74LVC1G17 Rev 16.1, Tables 7-8](https://assets.nexperia.com/documents/data-sheet/74LVC1G17.pdf),
[74LVC1G14](https://assets.nexperia.com/documents/data-sheet/74LVC1G14.pdf),
[DMN2056U, electrical characteristics](https://www.diodes.com/datasheet/download/DMN2056U.pdf)
and [TPS3808](https://www.ti.com/lit/ds/symlink/tps3808.pdf).
The following preserves the assumptions in
[PWR-004's raw RUN buffer and held shutdown clamp](main_regulator_ltc3119.md#raw-run-buffer-and-held-shutdown-clamp):
resistor endpoints `rmin=.99*.99`, `rmax=1.01*1.01`; raw maximum 4.6 V;
held minimum 2.7 V; four 1 kOhm/1 MOhm held gate branches, including the
reserved future USB-clear branch; 1 uA allocated adverse leakage per gate;
and 0.2 ohm installed Q3 on-resistance acceptance allocation.

R34's adverse MAIN_PWR_EN input sum remains 3.8 uA because U16 isolates
the converter EN load. The supervisor low-POR sink screen remains
`(1.3-.2)/(110k*rmin)+3.8uA = 14.003041 uA <15 uA`.
MAIN_PWR_EN is >=2.273598 V at held 2.7 V, and >=2.587304 V at the
3.263705831 V minimum-valid raw source with the allocated 0.250 V
raw-to-held drop. These are input-node voltage screens, not a substitution
of the converter's 1.2 V threshold for U16's Schmitt threshold. PWR-004's
correlated raw/held threshold-envelope review and its explicitly conditional
continuous-supply interpolation remain applicable.

For powered U16, its 100 uA output-test condition gives VOH >=VCC-0.1 V
and VOL <=0.1 V. The high/load calculation includes an additional 1 uA
allocated Q3 off-state drain current besides TI's 0.2 uA EN leakage.
DMN2056U specifies 1 uA IDSS at 25 C only; this installed hot-leakage
allocation is not a new manufacturer guarantee.

| Converter EN screen | Bound | Comparison |
| --- | ---: | --- |
| High, raw 3.2 V, `(3.2-.1-1.2uA*1k*rmax)/(1+1k*rmax/(68k*rmin))` | >=3.052060850 V | >1.2 V |
| U16 high-state static load, `4.6/(68k*rmin)+1.2uA` | <=70.220568 uA | <100 uA test condition |
| Powered low, `.1+.2uA*1k*rmax`, ignoring helpful pulldown/Q3 sinking | <=0.100204020 V | <0.4 V |
| Zero raw, `(2uA+1uA)*68k*rmax` | <=0.208100400 V | <0.4 V, conditional endpoint only |
| Held Q3 on, `(4.6/(1k*rmin)+1uA)/(1/(1k*rmin)+1/.2)`, ignoring R69 | <=0.000938689 V | <0.4 V, conditional clamp screen |

The zero-raw and clamped screens retain PWR-004's 1 uA adverse EN-source
allocation rather than claiming TI guarantees 0.2 uA below its electrical
test range. The zero-raw screen uses U16's 2 uA Ioff limit at VCC=0;
it does not describe the intervening unspecified U16 region below 1.65 V.
The held clamp is the independent shutdown action during that interval.
The final clamp screen conservatively also includes the adverse 1 uA
source term that was negligible but omitted from PWR-004's rounded result.

Four held branches retain a resistor-limited simultaneous charging screen
of 18.773595 mA, steady load 22.773595 uA and VGS >=2.596278 V.
The charging screen is below the inverter's 50 mA absolute output limit,
not an authorization for sustained operation at that limit. The 1.125 mA
held control budget retains 98.226405 uA after 500 uA reservoir leakage,
500 uA inverter non-rail-input allowance, 4 uA static inverter current and
the four branches; other already allocated held loads must fit that remainder.
Preserve the 10 ms complete response allocation and 1 uC transition-charge
reserve. These are schematic-level conditional screens, not measured
sequencing. Do not add the separately proposed eFuse/MR load or change the
source-sense point through this migration. Raw input protection remains
unresolved; the preserved native control connectivity has passed audit.

## Passive integration contract

The following table binds the migration choices to existing native references.
It matches the saved native implementation and audited BOM. No new passive
references are introduced; physical qualification remains open.

| Existing references | Pre-migration selection/function | Migration disposition |
| --- | --- | --- |
| L2 | Eaton EXLA1V0703-3R3-R, 3.3 uH | Replace with Coilcraft XGL5020-471MEC, 0.47 uH; conditional incoming/operating range below |
| C73 | TDK C3216X7R1V106K160AC, 10 uF | Retain at input |
| C96 | Murata GRM32ER71C226KEA8L, 22 uF | Replace with Samsung CL32B226MOJNNNE, 22 uF, at input |
| C74, C75 | Murata GRM32ER71C226KEA8L, 22 uF each | Replace with Samsung CL32B226MOJNNNE, 22 uF each, at output |
| C94 | TDK 100 nF LTC BST1/SW1 capacitor | Repurpose as third Samsung CL32B226MOJNNNE 22 uF output capacitor |
| C99, C100 | Panasonic EEF-JX0J151RF, 150 uF each | Retain as output bulk; loaded-startup/PDN qualification remains required |
| C93 | TDK C1608X7R1H104K080AA, 100 nF | Retain at input |
| C95 | TDK 100 nF LTC BST2/SW2 capacitor | Remove obsolete bootstrap component |
| C97 | TDK C3216X7R1V106K160AC, 10 uF, private LTC VCC | Remove; do not treat as removed main-output capacitance |
| R66 | 162 kOhm LTC RT programming resistor | Repurpose as 10 ohm series trim in feedback lower leg |
| R67, C98 | 42.2 kOhm VC resistor; 4.7 nF VC compensation capacitor | Remove obsolete external-compensation components |
| U16, C101, R68-R71, Q3 | Raw enable buffer/bypass and held clamp | Preserve wiring and selections; C101 is not output storage |

TI Section 8.3 requires at least 4 uF effective input capacitance and
21 uF effective output capacitance for this voltage, with 0.37..0.57 uH
effective inductance. Section 10.2.2.3 recommends two nominal 47 uF output
ceramics below 3.6 V and states no upper capacitance limit. That does not
establish startup timing, an arbitrary distributed network's phase margin,
or compliance with the product's discharge ceiling. The former LTC
external-compensation model and 250..850 uF model window are not a
TPS63806 stability qualification or a newly imposed TPS63806 upper limit.
The independent whole-main-rail 1 mF shutdown ceiling remains in force.

### Inductor qualification basis

The [Coilcraft XGL5020 primary specification](https://www.coilcraft.com/en-us/products/power/shielded-inductors/molded-inductor/xgl/xgl5020/)
lists XGL5020-471MEC as 0.47 uH +/-20%, tested at 1 MHz, 0.1 Vrms and
zero DC current. DCR is 4.3 mOhm maximum at 25 C. Its 6.4 A 10%-drop
and 10.7 A 20%-drop saturation references are 25 C conditions, not
all-temperature guarantees. Heating references likewise require application
verification. They do not remove the need to consider TI's maximum switch
limit when screening component stress.

The catalog zero-current interval is 0.376..0.564 uH. Merely multiplying
that by a +/-10% operating allowance gives 0.3384..0.6204 uH and does NOT
fit TI's 0.37..0.57 uH range. The prototype therefore requires an explicit
incoming acceptance interval of 0.412..0.518 uH, plus no more than +/-10%
additional installed operating change. This gives 0.3708..0.5698 uH.
Incoming screening/vendor control and the combined current, temperature,
frequency and aging behavior are qualification conditions, not a tighter
catalog tolerance hidden behind the same MPN. Final current, hot DCR and
loss qualification remain open; the exact new footprint is deferred.

The 2026-09-08 procurement checkpoint for
[DigiKey Marketplace 2457-XGL5020-471MEC-ND](https://www.digikey.com/en/products/detail/coilcraft/XGL5020-471MEC/16634589)
recorded 785 available, USD 3.83 / 3.83 / 3.345 at quantities 1 / 10 / 100,
plus a separate USD 10 shipping fee. The listing ships from Coilcraft in
approximately four days. This unreserved marketplace snapshot does not
promise the narrower incoming-inductance acceptance interval above.

### Samsung local-capacitance basis

The migration uses C74/C75/C94 as three Samsung **CL32B226MOJNNNE** output
capacitors and C96 as the same part at input. The
[Samsung product record](https://product.samsungsem.com/mlcc/CL32B226MOJNNN.do)
lists the exact E packaging suffix, mass production, 22 uF +/-20%, 16 V,
X7R and 1210 size. These replace the earlier unselected TDK candidate.

The primary typical bias-data interpolation at 3.393012496197 V gives
90.2215540272042% retention, at 25 C, 120 Hz and 0.5 Vrms excitation. This
corresponds to 19.848741885985 uF per nominal 22 uF part, before tolerance,
temperature or aging. Samsung explicitly identifies the page's data as
typical design-reference data. This is supporting output-bias evidence,
not an aging guarantee or evidence for C96 at raw input up to 4.6 V.
All 91 published samples are preserved in the
[2026-09-08 manufacturer curve CSV](../resources/datasheets/Samsung_CL32B226MOJNNNE_dc_bias_2026-09-08.csv).
The executable check below interpolates the bracketing 3.3 V and 3.4 V
samples directly; it does not substitute the different rounded 3.393 V point.

The conditional lower screen is `3*22uF*0.8*0.85*0.6 = 26.928 uF`:
0.8 is initial tolerance, 0.85 is the X7R temperature factor, and 0.6 is
an additional allocated combined bias/aging retention factor. The 0.6 term
is not a guaranteed Samsung characteristic. Thus 26.928 uF exceeds the 21 uF
requirement only if that retention allocation and relevant-frequency local
behavior are established for the installed parts. It is not a qualified
minimum. Under the same conditional factors, C96 alone gives 8.976 uF,
above TI's 4 uF input minimum before counting retained C73/C93; validate
the input retention over the actual raw-voltage range independently.
Startup, PDN, ripple and discharge acceptance remain open. No new capacitor
references or reset components are introduced by this migration contract.

### Source-charged storage and shutdown

PWR-004 inventories 147.21 uF directly on the main rail plus C43's 10 uF
behind FB1, before its added C99/C100 300 uF. The four C58-C61 button
filters add 0.40 uF through R27-R30; radio-on adds 10.20 uF separately.
Thus PWR-004's nominal examples are 457.61 uF cold/radio-off and 467.81 uF
radio-on. C74/C75 replacement leaves their nominal 44 uF unchanged. C94
adds 22 uF directly to main; its former bootstrap capacitor, C95 bootstrap,
C97 private VCC and C98 VC compensation capacitor were never included in
the main-output inventory. In particular, do NOT subtract C98's 4.7 nF.
The output change is exactly +22 uF, not +22 uF minus 4.7 nF.

| Required migrated inventory | Nominal capacitance |
| --- | ---: |
| Direct main, including retained bulk and new C94 | 469.21 uF |
| C43 through FB1 | 10.00 uF |
| Four button filters through 10k paths | 0.40 uF |
| Cold/radio-off source-charged total | 479.61 uF |
| Radio-on additional switched storage | 10.20 uF |
| Radio-on source-charged total | 489.81 uF |

These nominal totals match the independently audited native component inventory.
Input C73/C93/C96, raw C101 and removed private-LTC capacitors must be
inventoried separately for source-side startup charge. Their values do not
belong in the direct output-capacitance sum.

At the proposed full upper voltage, those nominal inventories store
1.627323 / 1.661931 mC and 2.760763 / 2.819477 mJ respectively. The
existing 1 mF whole-main-rail ceiling corresponds to 3.393012 mC and
5.756267 mJ. These charge/energy calculations do not imply that capacitance
behind a ferrite or resistor is available for an instantaneous load step.
Actual maximum stored charge, including tolerance, aging and external
backpower, must remain bounded; nominal sums cannot certify that ceiling.

Retain the external discharge circuit: TPS63806 does not provide the
TPS63807 output-discharge feature. PWR-004's conservative tail model uses
11.4211 ohm maximum discharge resistance, 1.5 mA key-filter return current,
3.6 V initial voltage and 0.3 V target. At 1 mF, including the 10 ms response
allocation, it gives 38.997458 ms versus the existing 46.589403 ms hold.
Keeping its 3.6 V initial bound is conservative for this proposal. New
backpower or changes to held loading require recalculation; no hold or
hard-off requirement is relaxed.

## Current, thermal and autonomous-startup boundaries

Nominal divider current is 495.049505 uA. Upper, lower-1000 and lower-10
resistor powers are 1.372414 mW, 0.245074 mW and 2.450740 uW respectively;
total divider loss is 1.619939222 mW. Include this persistent rail load in
low-power accounting and avoid double-counting it in a total allocation.

Retain the PWR-004 load scenarios as allocations, not converter ratings:
1.800 A continuous, 1.880 A radio-off cold reference, 2.250 A non-capacitive
cold ceiling and a 3.000 A temporary charging screen. At the full upper
voltage, 3.2 V input and an allocated 75% efficiency:

| Output current A | Output W | Source W | Source A | Total stage loss W |
| ---: | ---: | ---: | ---: | ---: |
| 1.800 | 6.107422 | 8.143230 | 2.544759 | 2.035807 |
| 1.880 | 6.378863 | 8.505151 | 2.657860 | 2.126288 |
| 2.250 | 7.634278 | 10.179037 | 3.180949 | 2.544759 |
| 3.000 | 10.179037 | 13.572050 | 4.241266 | 3.393012 |

The efficiency floor is not established by typical efficiency plots. Stage
loss includes IC and passive losses, not solely junction dissipation. TI's
78.8 C/W package thermal metric gives an illustrative 0.507614 W IC budget
from 85 C ambient to 125 C junction; actual board thermal impedance and
IC/passive loss partition require validation. The table neither qualifies
continuous thermal capability nor permits these currents from every source.
Include other separately powered product loads in the upstream budget.

A peak switch-current limit is not guaranteed output-current capability.
TI's TPS63806 boost-limit range is 4.4..6.25 A at VIN >=2.5 V; assess minimum
available current for operation and maximum current for component stress,
including ripple, temperature and every conversion mode. The selected L2
identity and conditional inductance window do not complete its current,
hot-DCR, saturation or loss qualification.

Startup must remain autonomous and radio-off. TI describes a current-limit
ramp, with 224 us ramp and 321 us enable delay listed as typical under
specific conditions; neither is a guaranteed minimum linear voltage ramp.
Do not derive guaranteed charging current as `C*V/224us`. VIN must exceed
1.8 V until power good; this is not permission to reduce the existing
source-valid threshold. Loaded cold/warm startup, source sag, monotonicity,
reset assertion/release and the MCU's supply-gradient constraints must be
verified with the completed passive network. Current and charge accounting
cannot replace that dynamic check.

## Reproducible arithmetic

This script checks design inputs, not the current native BOM. Run from the
repository root with Python 3; only the standard library is required.

```sh
python3 - <<'PY'
import csv
from decimal import Decimal
from math import isclose, log, prod

def endpoints(nominal, factors, absolute):
    assert nominal > 0 and all(0 <= x < 1 for x in factors)
    result = tuple(nominal*prod(1+s*x for x in factors)+s*absolute
                   for s in (-1, 1))
    assert 0 < result[0] < nominal < result[1]
    return result

upper = endpoints(5600, (.0002, 5e-6*100, .001, .0005), .02)
lower = endpoints(1000, (.0001, 2e-6*100, .001, .0005), .01)
trim = endpoints(10, (.01, 200e-6*100, .01), .05)
bottom = tuple(lower[i]+trim[i] for i in (0, 1))
assert bottom[1] < 100e3
nominal = .5*(1+5600/1010)
static = (.495*(1+upper[0]/bottom[1])-100e-9*upper[1],
          .505*(1+upper[1]/bottom[0])+100e-9*upper[1])
complete = (static[0]-.075, static[1]+.075)
assert isclose(nominal, 3.272277227722772, abs_tol=1e-12)
assert isclose(complete[0], 3.1518196800191163, abs_tol=1e-12)
assert isclose(complete[1], 3.3930124961973753, abs_tol=1e-12)
assert 3 < complete[0] < complete[1] < 3.6
print('PWR-006 nominal/static/complete V:', nominal, static, complete)
print('Upper/lower/trim resistance endpoints:', upper, lower, trim)
print('Conditional SDRAM high margin V:', 2.4-.7*complete[1])
divider_current = .5/1010
powers = tuple(divider_current**2*r for r in (5600, 1000, 10))
assert isclose(sum(powers), nominal*divider_current)
print('Nominal divider A / resistor W:', divider_current, powers)
assert isclose(sum(powers), .0016199392216449367, abs_tol=1e-15)
print('Nominal total divider W:', sum(powers))
for current in (1.8, 1.88, 2.25, 3):
    output_w = complete[1]*current
    source_w = output_w/.75
    print('Load A / output W / source W / source A / loss W:',
          current, output_w, source_w, source_w/3.2, source_w-output_w)
direct_uf = 147.21+300+22
cold_uf = direct_uf+10+.4
radio_uf = cold_uf+10.2
assert isclose(direct_uf, 469.21) and isclose(cold_uf, 479.61)
assert isclose(radio_uf, 489.81)
assert isclose(cold_uf-457.61, 22)  # C98 VC was never main-output storage.
for capacitance in (cold_uf*1e-6, radio_uf*1e-6, 1e-3):
    print('Inventory F / charge C / energy J:', capacitance,
          capacitance*complete[1], .5*capacitance*complete[1]**2)
tail = .010 + 11.4211*.001*log((3.6-11.4211*.0015)/(.3-11.4211*.0015))
assert isclose(tail, .038997458, abs_tol=1e-9)
assert tail < .046589403
print('Conservative 1mF shutdown s:', tail)
print('Illustrative IC thermal W at 85C:', (125-85)/78.8)
rmin, rmax = .99*.99, 1.01*1.01
por = (1.3-.2)/(110000*rmin)+3.8e-6
main_en_held = 2.7-3.8e-6*110000*rmax
main_en_source = 3.263705830523478-.250-3.8e-6*110000*rmax
en_leakage, q3_off_allocation = .2e-6, 1e-6
en_hi = (3.2-.1-(en_leakage+q3_off_allocation)*1000*rmax) / (
    1+1000*rmax/(68000*rmin))
en_lo = .1+en_leakage*1000*rmax
buffer_dc = 4.6/(68000*rmin)+en_leakage+q3_off_allocation
en_absent = (2e-6+1e-6)*68000*rmax  # No zero-VIN TI leakage guarantee.
en_clamped = (4.6/(1000*rmin)+1e-6)/(1/(1000*rmin)+1/.2)
gate_peak = 4*4.6/(1000*rmin)
gate_dc = 4*(4.6/(1e6*rmin)+1e-6)
vgs = (2.6-1e-6*1000*rmax)/(1+1000*rmax/(1e6*rmin))
control_spare = .001125-(.000500+.000500+.000004+gate_dc)
assert isclose(por, 14.00304050607081e-6) and por < 15e-6
assert isclose(en_hi, 3.052060849827375) and en_hi > 1.2
assert isclose(en_lo, .100204020) and isclose(en_absent, .208100400)
assert max(en_lo, en_absent, en_clamped) < .4
assert buffer_dc < 100e-6 and gate_dc < 100e-6
assert gate_peak < .050 and vgs > 2.5 and control_spare > 98e-6
print('MAIN_PWR_EN held/source V; supervisor POR sink A:',
      main_en_held, main_en_source, por)
print('Converter EN high/low/absent/clamped V; U16 static load A:',
      en_hi, en_lo, en_absent, en_clamped, buffer_dc)
print('Held gate peak/DC A, VGS V, held spare A:',
      gate_peak, gate_dc, vgs, control_spare)
output_local_uf = 3*22*.8*.85*.6
input_local_uf = 22*.8*.85*.6
assert isclose(output_local_uf, 26.928, abs_tol=1e-12)
assert output_local_uf > 21 and input_local_uf > 4
curve_path = ('ra8p1_kicad/resources/datasheets/'
              'Samsung_CL32B226MOJNNNE_dc_bias_2026-09-08.csv')
with open(curve_path, encoding='ascii', newline='') as curve_file:
    rows = csv.DictReader(line for line in curve_file if not line.startswith('#'))
    curve = [(Decimal(row['dc_bias_V']),
              Decimal(row['capacitance_change_percent'])) for row in rows]
assert len(curve) == 91
assert all(a[0] < b[0] for a, b in zip(curve, curve[1:]))
bias_v = Decimal('3.393012496197')  # Full upper endpoint to 12 decimal places.
a, b = next((a, b) for a, b in zip(curve, curve[1:])
            if a[0] <= bias_v <= b[0])
assert (a[0], b[0]) == (Decimal('3.3'), Decimal('3.4'))
delta_c_percent = a[1]+(bias_v-a[0])*(b[1]-a[1])/(b[0]-a[0])
typical_retention = 1+delta_c_percent/100  # Typical data, not a limit.
assert abs(typical_retention-Decimal('0.9022155402720422279814285171')) < Decimal('1e-26')
assert abs(22*typical_retention-Decimal('19.848741885984929')) < Decimal('1e-15')
print('Samsung local output/input conditional uF:', output_local_uf, input_local_uf)
print('Samsung CSV samples / interpolation V / typical retention:',
      len(curve), bias_v, typical_retention)
print('Samsung typical output-biased uF each:', 22*typical_retention)
catalog_l = (.47*.8, .47*1.2)
unrestricted_operating_l = (catalog_l[0]*.9, catalog_l[1]*1.1)
incoming_l = (.412, .518)
operating_l = (incoming_l[0]*.9, incoming_l[1]*1.1)
assert unrestricted_operating_l[0] < .37 and unrestricted_operating_l[1] > .57
assert isclose(operating_l[0], .3708) and isclose(operating_l[1], .5698)
assert .37 <= operating_l[0] < operating_l[1] <= .57
print('L2 catalog / unrestricted operating / accepted operating uH:',
      catalog_l, unrestricted_operating_l, operating_l)
print('PASS arithmetic only; hardware qualification remains pending.')
PY
```

## Acceptance boundary

This basis supports the audited native regulator checkpoint, not a finished
product voltage contract. Passive implementation, logical pin mapping and
held-enable connectivity have passed differential audit. RST-002 additionally
records the targeted MCU reset connectivity check. Resolve radio DC release
and full-system reset/fault coordination before claiming a complete power
design. Keep the BOM, ERC, netlist
and full PDF synchronized at each checkpoint. Footprint and layout
qualification remain deferred.
Bench release additionally requires source protection/budget, all-corners
rail behavior, loaded startup, relevant-frequency PDN/loop response,
temperature and hard-off/backpower qualification. #846 is not closed by
nominal divider arithmetic alone.
