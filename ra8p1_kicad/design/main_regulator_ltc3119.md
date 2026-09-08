# PWR-004: LTC3119 main-regulator migration design basis

**Historical regulator checkpoint, superseded 2026-09-08 by
[PWR-006 TPS63806](main_regulator_tps63806.md).** The native U13 stage is
now TPS63806, not LTC3119. Retained buffer, held-clamp and discharge parts
still cite this record for their original derivation; PWR-006 rechecks
their EN interface and preserves their shutdown allocations. LTC3119
divider, compensation, startup and stability calculations below do not
describe or qualify the current regulator. Reset coordination, source
protection and physical qualification remain open.

Revision 1, 2026-09-08. Tracking: [power #825](https://github.com/bsikar/ra8-firmware/issues/825)
and [architecture #823](https://github.com/bsikar/ra8-firmware/issues/823).
This record defines the calculated circuit basis for migration of U13 to
LTC3119IUFD#PBF. The native power-stage wiring has been saved and its
intermediate independent connectivity audit passed. The bound power-stage
references below describe that draft checkpoint. The RUN buffer/clamp and
R34 110 kOhm change are now implemented and saved in the native schematic;
the independent read-only control connectivity audit passed: 210 components,
312 nets and all 309 prior net partitions preserved. Active native ERC reports
133 errors and two existing warnings, with no added findings in this change;
this is a differential check, not a clean ERC or hardware qualification.
Source protection is not implemented, and the full migration remains
incomplete. Checkpoint review verified all 207 included references in the
18-column native BOM against schematic properties. The full 11-page PDF
was exported and visually reviewed; stale radio voltage notes and an
overlapping RUN-control label were corrected in native KiCad.
This record does not assert complete whole-product connectivity or hardware
qualification.

Rail adoption is blocked by the unresolved
[guaranteed SDRAM logic-high conflict #846](https://github.com/bsikar/ra8-firmware/issues/846),
affecting power #825 and memory #827 under epic #821. The 3.4185 V nominal
proposal and its calculations below remain an analyzed draft, not an accepted
rail contract: at its 3.584411411 V complete upper envelope, U14
IS42S32160F-7TLI's guaranteed 2.4 V output high is 109.088 mV below the
RA8P1 SDRAM input-high requirement of 0.7 times VCC/VCC2. This is a
primary-specification gap, not an observed board failure. The illustrative
3.25..3.35 V resolution target in #846 is not achieved performance and is
not substituted into this proposal's arithmetic. The independent RUN-control
work can proceed, but does not resolve this voltage conflict.

The design incorporates the reviewed
[voltage and RUN proposal](https://github.com/bsikar/ra8-firmware/issues/825#issuecomment-5585180693),
[passive and compensation proposal](https://github.com/bsikar/ra8-firmware/issues/825#issuecomment-5585427146),
and the held RUN clamp and corrected digital-power arithmetic from the
[subsequent protection review](https://github.com/bsikar/ra8-firmware/issues/825#issuecomment-5586557672).
The [VBATT and supply-gradient review](https://github.com/bsikar/ra8-firmware/issues/825#issuecomment-5586735632)
adds the conditional firmware/option invariant and key-filter return-current
correction below; the firmware invariant remains unimplemented.
The protection review's TPS259470ARPWR post-charger eFuse, 680 ohm ILM setting,
4.0 A / 4.25 A combined source envelope, FLT-to-MR connection, and additional
150 uA held fault load remain a **separate, unadopted proposal**. None is
implemented or approved by this main-regulator design basis. In particular,
this record does not move the source-sense point or remove U11 MR's existing
held-supply connection.

This basis is intended to replace the TPS63802-specific selection in
[PWR-002](power_decoupling.md#pwr-002-main-rail-regulation-and-reset-headroom)
as migration is completed. Older saved implementation checkpoints in that
record and [SYS-007](system_power_design.md) describe their named snapshots.
Dependent voltage, reset, source-power and shutdown records must be updated
with the native migration; a changed regulator name alone does not perform
that update. No fabrication approval or completed whole-product budget is
implied.

## Circuit and exact components

The primary regulator reference is [ADI LTC3119 Rev B](https://www.analog.com/media/en/technical-documentation/data-sheets/3119fb.pdf),
especially printed pp.2-4, 12-24 and the applications on pp.27-28. Use the
I-grade QFN part LTC3119IUFD#PBF, not a package or temperature-grade substitute.
The regulator is configured autonomously; its startup does not depend on
firmware programming. Mandatory radio-off cold startup and the separately
supplied audio/display/camera/lighting domains remain in force.

| Native reference | Function | Exact part and required connection |
| --- | --- | --- |
| U13 | Main regulator | LTC3119IUFD#PBF; VIN and both PVIN pins to SYS_AON; both PVOUT pins to +3V3_MCU; all PGND pins and exposed pad to GND; SGND to the quiet ground return |
| L2 | SW1-to-SW2 inductor | Eaton EXLA1V0703-3R3-R, 3.3 uH; connect both same-name switch pins on each side |
| R41 | Feedback upper resistor | Susumu RG1608P-333-B-T5, 33.0 kOhm, +3V3_MCU sense point to FB |
| R42 | Feedback lower resistor | YAGEO RT0603BRD0710KL, 10.0 kOhm, FB to SGND |
| R66 | Oscillator RT resistor | YAGEO AC0603FR-07162KL, 162 kOhm, RT to SGND |
| R67 | VC compensation resistor | YAGEO RT0603BRD0742K2L, 42.2 kOhm, VC to compensation-capacitor node |
| C98 | VC compensation capacitor | TDK C1608C0G1H472J080AA, 4.7 nF, compensation-resistor node to SGND; series RC, not separate shunts |
| C74, C75 | Local output ceramics | Two Murata GRM32ER71C226KEA8L, 22 uF each, +3V3_MCU to GND |
| C99, C100 | Additional output bulk | Two Panasonic EEF-JX0J151RF, 150 uF / 6.3 V each; positive to +3V3_MCU, negative to GND |
| C73 | PVIN bypass, 10 uF | TDK C3216X7R1V106K160AC, PVIN to GND |
| C96 | PVIN bypass, 22 uF | Murata GRM32ER71C226KEA8L, locally from PVIN to GND |
| C97 | Private internal VCC bypass | C3216X7R1V106K160AC, 10 uF, internal VCC to GND; this is not a main-output capacitor or connection to a board-wide VCC rail |
| C94 | BST1 bootstrap bypass | C1608X7R1H104K080AA, 100 nF, BST1 to SW1 |
| C95 | BST2 bootstrap bypass | C1608X7R1H104K080AA, 100 nF, BST2 to SW2 |
| C93 | VIN local bypass | C1608X7R1H104K080AA, 100 nF, VIN to GND |
| U13 control pins | Mode and unused MPPC | PWM/SYNC, SVCC and MPPC to private internal VCC; no VOUT-to-VCC bootstrap diode; no initially populated feedback feedforward capacitor |
| U13 unused pins | PGOOD / NC | PGOOD is unused unless a separately reviewed interface is added; do not substitute it for the existing supervisors; leave package NC pins unconnected |

Native pin numbers and the QFN exposed pad must be verified against the
manufacturer pin configuration before declaring the circuit complete.
SGND is a quiet return within the same ground system, not an isolated
floating ground. The divider bottom, RT and compensation return belong
there. The later PCB must keep feedback sensing and control returns away
from the switch-current loops, place local bypass loops at their pins, and
provide exposed-pad heat spreading and vias. The old 0.47 uH TPS63802
inductor must not be retained for this circuit.

### Raw RUN buffer and held shutdown clamp

The following control circuit and R34 110 kOhm change are implemented and
saved in the native migration draft. POWER_OFF_H is carried from the source
control sheet through the root hierarchy to the main-regulator sheet. The
separate independent read-only control connectivity audit passed for these
additions, including preservation of all prior net partitions. This checkpoint
does not resolve #846 or adopt the proposed rail envelope.

| Native reference | Function | Exact part and required connection |
| --- | --- | --- |
| U16 | Raw-powered noninverting Schmitt buffer | Nexperia 74LVC1G17GW,125: pin 1 NC, pin 2 A to MAIN_PWR_EN, pin 3 GND, pin 4 Y to R68, pin 5 VCC to raw SYS_AON |
| C101 | Buffer bypass | C1608X7R1H104K080AA, 100 nF, buffer VCC to GND |
| R68 | RUN series resistor | RC0603FR-071KL, 1 kOhm, raw buffer Y to LTC3119 RUN, QFN pin 15 |
| R69 | RUN pulldown | RC0603FR-0768KL, 68 kOhm, converter-side RUN to GND |
| R34 | Held MAIN_PWR_EN pullup | RC0603FR-07110KL, 110 kOhm, preserving its existing AON_HOLD-to-MAIN_PWR_EN connection |
| Q3 | Held RUN clamp | DMN2056U-7: drain pin 3 to converter-side RUN after R68; source pin 2 to GND; gate pin 1 to its own gate network |
| R70 | RUN-clamp gate resistor | RC0603FR-071KL, 1 kOhm, POWER_OFF_H to the clamp gate |
| R71 | RUN-clamp gate pulldown | RC0603FR-071ML, 1 MOhm, clamp gate to GND |

Keep the existing held U12 inverter, KILL and main-discharge connections.
RUN, KILL, discharge and future SYS-009 USB-permission-clear FET drains
remain separate. Count four held inverter gate branches in the allocation,
including the future USB-permission-clear branch. This is a load allowance,
not a statement that every branch is already placed.

The raw buffer avoids circular startup from a switched supply. Its output
is not specified over the entire raw-supply interval below 1.65 V. Internal
LTC3119 VCC can retain charge during raw collapse, so converter UVLO and
an endpoint calculation at zero raw voltage do not close that interval.
The independently held clamp supplies the off-state action while held
control remains valid. Relevant primary references are
[74LVC1G17 Rev 16.1](https://assets.nexperia.com/documents/data-sheet/74LVC1G17.pdf),
[74LVC1G14](https://assets.nexperia.com/documents/data-sheet/74LVC1G14.pdf),
[DMN2056U](https://www.diodes.com/datasheet/download/DMN2056U.pdf), and
[TPS3808](https://www.ti.com/lit/ds/symlink/tps3808.pdf).

The buffer changes the adverse MAIN_PWR_EN input-current sum to 3.8 uA.
With initial 1% and an additional 1% resistor temperature allowance,
R34 = 110 kOhm gives 14.003041 uA supervisor low-POR sink current, below
its 15 uA test condition. At held 2.7 V, MAIN_PWR_EN screens at 2.273598 V;
at minimum valid raw 3.263705831 V and allocated 0.250 V raw-to-held drop,
it screens at 2.587304 V. Compare correlated raw/held states against the
buffer threshold envelope; continuous-supply interpolation is an engineering
screen, not a new threshold specification.

RUN screens at <=0.101020 V for a powered buffer low and >=3.052262 V
for high at raw 3.2 V. The buffer's static output load screens at
70.021 uA, within the 100 uA output-voltage test condition. At fully absent
raw supply, 2 uA buffer Ioff plus an allocated 1 uA adverse RUN source
current through 68 kOhm gives <=0.208100 V. The 1 uA term is not a
published hot RUN-current maximum; this endpoint screen does not replace
the held clamp.

With four 1 kOhm / 1 MOhm gate branches, raw/held maximum 4.6 V and an
allocated 1 uA adverse leakage per gate, the simultaneous resistor-limited
gate-charge screen is 18.773595 mA and steady loading is 22.773595 uA.
The former is below the inverter's 50 mA absolute output limit, but does
not authorize sustained operation at that limit. At held supply >=2.7 V,
the 100 uA high-output condition gives VGS >=2.596278 V. Using an installed
FET resistance acceptance allocation of 0.2 ohm, an adversarial 4.6 V raw
buffer gives RUN <=0.939 mV with the clamp asserted.

The existing 1.125 mA held control budget retains 98.226405 uA after
500 uA reservoir leakage, 500 uA inverter non-rail-input allowance, 4 uA
static inverter current and four gate branches. This remainder must cover
the other held-island loads already allocated by SYS-007. Neither the
1 uA gate leakage nor 0.2 ohm installed resistance is asserted as a
manufacturer all-temperature maximum. Preserve the shared 10 ms complete
response allocation and 1 uC transition-charge reserve. The unadopted
eFuse's extra MR/FLT load is not included in this budget.

## Voltage envelope and full-upper source accounting

The 33.0 kOhm / 10.0 kOhm divider gives 3.4185 V nominal. Both resistors
have initial 0.1% tolerance and 25 ppm/C TCR. Sources:
[Susumu RG specification](https://www.susumu.co.jp/common/pdf/n_catalog_partition01_en.pdf)
and [YAGEO exact 10 kOhm record](https://www.yageogroup.com/component-documentation/download/specsheet/RT0603BRD0710KL).
Use the I-grade 0.779..0.811 V feedback screen at its stated electrical-table
conditions, 100 C resistor excursion, and 100 nA total adverse FB-current
allocation. Feedback current is not specified as an all-temperature 100 nA
maximum; the allocation includes board effects and requires qualification.

| Quantity | Calculated screen |
| --- | ---: |
| Nominal output | 3.418500 V |
| Static lower / upper | 3.328456349 / 3.509411411 V |
| Combined remaining regulation, routing, ripple and transient allowance | +/-75 mV |
| Complete lower / upper envelope | 3.253456349 / 3.584411411 V |
| Margin above U2 maximum rising threshold 3.193951250 V | 59.505099 mV |
| Margin above U6 maximum rising threshold 3.139621574 V, after 87.5 mV switch drop | 26.334775 mV |
| Headroom below 3.6 V supply ceiling | 15.588589 mV |

The 75 mV term is a combined acceptance budget, not a separate allowance for
each error and not an observed waveform. It supersedes the candidate's
earlier 50 mV total allowance. Apply the same voltage envelope to every
connected load and interface, including radio switch drop and reset release.

Use the full upper endpoint, not just the static upper endpoint, in source
accounting. At allocated 75% efficiency and 3.2 V converter input:

| Output-current scenario | Output W at 3.584411411 V | Source W | Source A at 3.2 V | Total stage loss W |
| --- | ---: | ---: | ---: | ---: |
| 1.800 A continuous allocation | 6.451941 | 8.602587 | 2.688309 | 2.150647 |
| 2.250 A non-capacitive cold-load allocation | 8.064926 | 10.753234 | 3.360386 | 2.688309 |
| 3.000 A temporary design screen | 10.753234 | 14.337646 | 4.480514 | 3.584411 |

The 1.8 A source figure is 8.602587 W, not the 8.422587 W obtained from
the static upper voltage alone. Total stage loss includes IC and external
passive loss; it is not all assigned to the IC. The 3 A screen establishes
neither continuous thermal rating nor source permission. A full product
budget must add every separately powered audio, display, camera, lighting,
storage and control load, with their simultaneous modes and transients.
Do not double-count loads already included in the digital allocation.
USB-only 500 mA is not sufficient for this worst-case digital allocation.
No pack-protection coordination is inferred from an inductor current limit.

## Passive selection and capacitance accounting

The [Eaton ELX1222 data sheet](https://www.eaton.com/content/dam/eaton/products/electronic-components/resources/data-sheet/eaton-exla1v07-automotive-high-current-molded-inductor-data-sheet-elx1222-en.pdf)
lists 3.3 uH +/-20%, 18 mOhm maximum DCR at 25 C, a 10 A heating reference
and 13 A saturation reference for EXLA1V0703-3R3-R. The references correspond
to approximately 40 C rise and 30% inductance reduction at the specified
conditions, not all-temperature installed limits. Body height is 3.1 mm;
the recommended footprint envelope is 8.7 by 8.3 mm. The loop model allocates
L <=4.4 uH; the current screen allocates L >=2 uH and f >=400 kHz.

RT = 162 kOhm gives about 494.071 kHz. Initial 1% and 100 ppm/C across
100 C give a resistor-only 484.713..503.699 kHz screen, excluding oscillator
variation. AC0603FR-07162KL is the specific selected series, not the
out-of-stock RC-series substitute from the sourcing checkpoint. The
[YAGEO AC specification](https://yageogroup.com/content/datasheet/asset/file/PYU-AC_51_ROHS_L)
and [YAGEO RT specification](https://yageogroup.com/content/datasheet/asset/file/PYU-RT_1-TO-0-01_ROHS_L)
support the programming and compensation resistor choices.
[TDK's compensation-capacitor record](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1608C0G1H472J080AA)
specifies 4.7 nF +/-5%, C0G 0 +/-30 ppm/C, 50 V, -55..125 C.

Each EEF-JX0J151RF is 150 uF +/-20%, rated 6.3 V, with 5 V category
voltage at 125 C, 15 mOhm maximum initial ESR at 100 kHz / 20 C,
94.5 uA initial leakage limit and 3000 h / 125 C endurance. Two add
189 uA initial leakage allowance, which must remain in low-load accounting.
The 5.1 A ripple reference is multiplied by 0.7 above 45 C through 85 C
and by 0.25 above 85 C through 125 C. Each case is 7.3 by 4.3 by 1.9 mm.
Endurance allows +/-20% capacitance change from initial; the 6.3 V damp-heat
table allows +70%/-20%. These condition-specific endpoints are not
simultaneous installed guarantees. Sources:
[Panasonic JX catalog dated 2026-09-01](https://industrial.panasonic.com/cdbs/www-data/pdf/ABE0000/ast-ind-199072.pdf)
and [exact product record](https://industrial.panasonic.com/ww/products/pt/sp-cap/models/EEFJX0J151RF).

The existing source-charged cold inventory at
[c1a141e19046](https://github.com/bsikar/ra8-firmware/commit/c1a141e19046e968950817d1bdbb2afd0302cabd)
is 157.21 uF nominal, including NOR C89-C92. It comprises **147.21 uF
directly on the main rail plus 10 uF C43 through FB1**. C43 contributes
storage, but the ferrite prevents treating it as an ideal parallel capacitor
at every frequency. The four 100 nF user-button filters add 0.4 uF storage
through their 10 kOhm paths; they are not instantaneous parallel loop
capacitance. Account for that extra storage conservatively in shutdown and
source charging. Their separate node voltages and RC decay require checking
when assessing every supply/interface after shutdown.

| Inventory | Nominal amount |
| --- | ---: |
| Prior direct main-rail capacitors | 147.21 uF |
| C43 behind FB1 | 10.00 uF |
| New directly connected polymer bulk | 300.00 uF |
| Cold main-source-charged subtotal, before button filters | 457.21 uF |
| Additional four resistively coupled button filters | 0.40 uF |
| Cold storage total including button filters | 457.61 uF |
| Radio-on additional switched capacitance | 10.20 uF |
| Radio-on storage total including button filters | 467.81 uF |

Radio-off cold startup must exclude the radio's 10.2 uF. Internal LTC3119
VCC's 10 uF, its 100 nF bootstrap capacitors, the VIN/PVIN input bypasses
and the raw-buffer bypass are not directly on +3V3_MCU. Inventory their
source-side startup charge separately; do not add their nominal values to
the main-output compensation capacitor sum.

The reviewed partial lower main-capacitance screen, excluding button filters,
is 280.18639 uF: 110.21u*0.9*0.85*0.6 + 47u*0.8 + 300u*0.8*0.8.
This is an inventory calculation, not proof that ferrite-separated capacitance
is fully visible to the regulator at crossover. The deliberately expanded
radio-on upper screen is 820.71865 uF using the polymer damp-heat increase.
The proposed **250..850 uF effective qualification window** remains the
loop model's input window; qualify actual relevant-frequency behavior and
all stored charge, including the filter-node additions. The whole-main-rail
hard-off ceiling remains 1 mF. Neither the partial screens nor the new bulk
establish that ceiling at every history/temperature corner.

The 32 uF nominal PVIN bypass gives 14.688 uF under the existing
0.9*0.85*0.6 residual-capacitance allocation. A minimum 10 uF effective
local PVIN bypass is an acceptance requirement. The 10 uF VCC bypass is a
nominal choice within the manufacturer's described broad stable range;
it is not a claim of a guaranteed 4.7 uF effective minimum. The reused
[TDK 10 uF](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C3216X7R1V106K160AC),
[TDK 100 nF](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1608X7R1H104K080AA),
and [Murata 22 uF](https://www.murata.com/en-us/products/productdetail?partno=GRM32ER71C226KEA8%23)
identities are unchanged by reuse; bias, temperature, aging and ripple
remain relevant to effective values.

## Loop model and acceptance limits

The circuit uses a series RC from VC to SGND. The simplified average-current
model uses gm = 120 uS, REA = 5 MOhm and nominal current-command gain
10.8 A/V. Gain is additionally swept +/-20% as sensitivity analysis only,
not a manufacturer tolerance or complete silicon corner coverage. Boost
efficiency is allocated at 0.75 and inductance at its 4.4 uH upper screen.

For incremental load admittance Y:

```text
Zcomp = REA*(1+s*Rz*Cp)/(1+s*(REA+Rz)*Cp)
Zout = 1/(Y+s*C)
T = gm*Zcomp*G*beta*Zout*(1-s/wRHP)
beta = 0.795/Vout
G = 10.8 (buck); 10.8*(Vin/Vout)*0.75 (boost)
wRHP = Vin^2*(Vout/Iout)/(Vout^2*L); omit in buck
Y = +Iout/Vout, 0, -Iout/Vout
```

The three admittances screen resistive, constant-current and constant-power
behavior. Entire-load constant-power behavior is deliberately pessimistic
relative to the current internal-converter allocation, but not a physical
model of every peripheral. The use of beta = 0.795/Vout is the reviewed
nominal-reference model approximation; actual divider/reference corners
and their correlations remain part of detailed simulation and qualification.

The 2160 enumerated combinations cover three capacitances, five input
voltages, both static output endpoints, three operating currents, three load
types, external R/C tolerance-temperature endpoints and gain sensitivity.
They give 1.416844..9.965519 kHz crossover, minimum modeled phase margin
51.167378 degrees, stable modeled closed-loop poles, and a maximum modeled
0.5 A disturbance of 66.112212 mV. The approximate inner-loop-separation
bandwidth ceiling at 4.4 uH is 10.681818 kHz. Only 8.887788 mV of the
75 mV combined allowance remains after that deliberately pessimistic
disturbance case; ripple, routing and additional regulation effects must
fit the same total envelope.

This model omits ESR/ESL, ferrite and distributed PDN resonances, switching
and current-loop delay, detailed buck-boost crossover behavior and
large-signal saturation. The static endpoint sweep is not a guarantee
over every instantaneous voltage or intermediate component value. The
linear model's 0.5 A result does not authorize larger instantaneous steps
or establish startup behavior. Its numerical stability supports drawing
the prototype circuit; time-domain and loop measurements on the later
PCB remain required for release.

## Startup, current and shutdown screens

The 1.800 A continuous allocation and 1.880 A radio-off cold reference sum
are unchanged. The 2.250 A non-capacitive cold-load ceiling remains an
acceptance allocation, not a measured maximum Renesas startup current.
Do not add the MCU's entire steady current again to its DCDC startup
reference. A 3 A available-output target supplies charging headroom but
requires source and thermal verification for the actual startup duration.

For 3 A at the static upper 3.509411 V, VIN = 3.2 V and 75% allocated
efficiency, the input-current screen is 4.386764 A. At L >=2 uH,
f >=400 kHz, and tLOW = 0 in the ADI ripple expression, ripple is
0.352665 A p-p and screened peak is 4.563097 A. DCR-only loss at
18 mOhm / 25 C is about 0.347 W, excluding hot DCR and core loss.
At the complete 3.584411 V upper endpoint the corresponding average input,
ripple and peak are approximately 4.480514 A, 0.428981 A p-p and
4.695005 A. These support normal-load inductor selection, not an all-state
short-circuit or switch-current guarantee. Include the full source range
and buck-mode ripple when completing the later physical current review.

At 850 uF the static-upper stored energy is 5.234287 mJ. Including the
complete upper voltage gives 5.460402 mJ. A purely illustrative linear
6 ms rise to the static upper voltage would charge 850 uF at 0.497167 A;
that example is not used as a startup proof. The published soft-start is
a typical internal current-command ramp, not a guaranteed minimum linear
output-voltage ramp. Actual autonomous loaded startup, monotonicity and
source sag must meet the MCU supply requirements without firmware
preconfiguration.

For the slowest tail, use zero downstream load, the existing 11.4211 ohm
maximum discharge-path allowance and a **1.5 mA total sustained key-filter
return-current allocation**. This accounts conservatively for C58-C61
returning charge through R27-R30 during collapse. Any additional external
drive/backpower requires a revised bound. With return current Ireturn:

```text
t_total = 10 ms + Rmax*C*ln((3.6-Rmax*Ireturn)/(0.3-Rmax*Ireturn))
```

| Main-capacitance ceiling | Total to 0.3 V, including 10 ms response | Margin against existing 46.589403 ms hold | Margin against separate proposed 43.248862 ms hold |
| --- | ---: | ---: | ---: |
| 850 uF | 34.647839 ms | 11.941564 ms | 8.601023 ms |
| 1 mF | 38.997458 ms | 7.591945 ms | 4.251405 ms |

These corrected times supersede the zero-return 34.123312 / 38.380367 ms
screens as the key-filter-aware tail allocation. The existing held budget
is 1.125 mA control plus 0.817 mA reverse-current allocation. The separate,
unadopted eFuse proposal adds 0.150 mA MR/FLT load and produces the shorter
43.248862 ms hold, rounded to 43.249 ms in the archived review. Its hold
margin is shown for comparison only; the MR connection and additional load
are not adopted here. The 0.4 uF filter-node storage remains inventoried
without asserting that a single lumped RC predicts each delayed node.
No response allocation or hard-off requirement is relaxed.

### VBATT profile, supply gradients and cold rearm

The reviewed native export connects U1.M16 VBATT to +3V3_MCU with VCC;
there is no separate backup battery. Physical shorting alone does not
establish the disabled-backup-switch profile.
[RA8P1 datasheet R01DS0439EJ0130 Rev.1.30](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet),
Tables 2.40/2.41 and Figure 2.24, pp.98-99, expresses gradients as
**dt/dV**, not dV/dt: startup crossing VPOR1 requires 0.0084..20 ms/V;
power-off crossing VPOR1 requires >=0.0084 ms/V with VBATT disabled or
>=1 ms/V with VBATT enabled. An operational change exceeding +/-10%
without crossing VPOR1 separately requires >=1 ms/V. A brownout/rebound
does not become exempt because shutdown was intended. Table 2.118,
pp.258-259, supplies the cold-rearm basis: conservatively retain VCC below
VPOR1(min) = 1.52 V for >=2 ms. The existing 200 ms minimum rearm
interval has arithmetic room only if discharge and hold actually complete.

The [RA8P1 HUM R01UH1064EJ0130 Rev.1.30](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
section 12.3.2 and pp.285, 511-512, 518 and 522, requires BPWSWSTP=1
when the VCC/VBATT switch is unused. Ordinary resets restore BPWSWSTP=0;
VDETE retains state except at VBATT_POR, which initializes it to zero.
Consequently a pre-shutdown BPWSWSTP write alone is not reset-safe.

The **proposed, unimplemented no-backup-switch firmware/option invariant** is:

1. Provision from a genuine cold-off state. Preserve VDETE=0 in every
   bootloader, application, service and low-power path; never enable it.
2. Program and verify effective secure/non-secure OFS1.PVDLPSEL=1, so
   Deep Software Standby1/2 does not select PVD0 for backup-switch control.
   This requirement is distinct from PVDAS and does not direct disabling
   useful voltage-monitor resets.
3. On every boot follow HUM 12.3.7.3 no-switch initialization: set
   BPWSWSTP=1 before clearing VDETE and initializing backup/RTC state.
   Exclude backup-switch enable helpers from this product profile.
4. Audit option attribution and every executable boot path. The archived
   review found no RA8P1 boot integration of the available no-switch helper;
   the helper alone does not implement the full sub-clock/RTC sequence.

The reset-window argument is an engineering inference from the independent
controls: even when BPWSWSTP resets, the dedicated detector remains disabled
and the PVD0 alternate remains unselected. It must be verified; this record
does not claim that existing firmware implements it or that shorted pins
alone qualify the disabled profile.

For the fastest loaded fall use Rmin = 10.7811 ohm, zero FET resistance,
and dt/dV = C/(Iload+V/Rmin). At the full-upper 3.584411 V and a retained
1.8 A load, 250..850 uF gives 0.117235..0.398598 ms/V; even 1 mF gives
only 0.468939 ms/V. The enabled-VBATT 1 ms/V profile therefore fails this
allocation. It would require >=2132.472 uF, incompatible with the 1 mF
ceiling. At 250 uF, enabled operation instead requires total falling-rail
current <=250 mA, including bleeder, loads and reverse sinking, implying
a different hardware isolation/sequencing/discharge design. A cooperative
shutdown hook does not cover unexpected loss or reset.

The proposed disabled profile needs >=17.913 uF for that same 1.8 A
model to meet 8.4 us/V, comfortably below the proposed 250 uF minimum.
That load allocation must actually bound fault-transition and reverse-sink
current. For startup over 250..850 uF, a sufficient pointwise net charging
current screen is 42.5 mA..29.7619 A after every instantaneous load. With
2.25 A non-capacitive startup load, the lower bound requires >=2.2925 A
delivery while applicable. Qualify capacitance effective on the ramp
timescale, not remote storage hidden behind impedance. Require monotonic
power-on, completed power-off below 0.3 V without backpower, and compliance
with the separate operational-gradient rule for interrupted/rebounding
excursions. The typical LTC soft-start does not establish these limits.

## Sourcing evidence

The following are archived, unreserved distributor snapshots from
2026-09-08 in the linked review comments. They are not a fresh inventory
check at fabrication time, a purchase or a lifetime-availability promise.
USD pricing excludes shipping and taxes; a dash means the cited checkpoint
did not record that quantity. Reuse exact manufacturer identity rather than
substituting a similarly named stocked series.

| Exact MPN | Distributor order code / source | Archived stock | USD qty 1 / 10 |
| --- | --- | ---: | ---: |
| LTC3119IUFD#PBF | [505-LTC3119IUFD#PBF-ND](https://www.digikey.com/en/products/detail/analog-devices-inc/LTC3119IUFD-PBF/6419504) | 7024 | 24.07 / - |
| RG1608P-333-B-T5 | [RG16P33.0KBCT-ND](https://www.digikey.com/en/products/detail/susumu/RG1608P-333-B-T5/1240562) | 18305 | 0.11 / - |
| RT0603BRD0710KL | [YAG1236CT-ND](https://www.digikey.com/en/products/detail/yageo/RT0603BRD0710KL/1072198) | 145117 | 0.10 / - |
| EXLA1V0703-3R3-R | [283-EXLA1V0703-3R3-RCT-ND](https://www.digikey.com/en/products/detail/eaton-electronics-division/EXLA1V0703-3R3-R/16893528) | 1393 | 1.44 / 1.182 |
| EEF-JX0J151RF | [10-EEF-JX0J151RFCT-ND](https://www.digikey.com/en/products/detail/panasonic-industry/EEF-JX0J151RF/16718126) | 9329 | 3.96 / 2.645 |
| AC0603FR-07162KL | [13-AC0603FR-07162KLCT-ND](https://www.digikey.com/en/products/detail/yageo/AC0603FR-07162KL/18105108) | 23478 | 0.11 / 0.029 |
| RT0603BRD0742K2L | [YAG4556CT-ND](https://www.digikey.com/en/products/detail/yageo/RT0603BRD0742K2L/6616712) | 22774 | 0.10 / 0.067 |
| C1608C0G1H472J080AA | [445-7400-1-ND](https://www.digikey.com/en/products/detail/tdk-corporation/C1608C0G1H472J080AA/2732835) | 63481 | 0.19 / 0.11 |
| 74LVC1G17GW,125 | [1727-4117-1-ND](https://www.digikey.com/en/products/detail/nexperia-usa-inc/74LVC1G17GW-125/1965408) | 44256 | 0.10 / - |
| RC0603FR-0768KL | [311-68.0KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0768KL/727352) | 106781 | 0.10 / - |
| RC0603FR-07110KL | [311-110KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-07110KL/726905) | 214574 | 0.10 / - |
| DMN2056U-7 | [DMN2056U-7DICT-ND](https://www.digikey.com/en/products/detail/diodes-incorporated/DMN2056U-7/7352909) | 80160 | 0.39 / 0.238 |
| RC0603FR-071KL | [311-1.00KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-071KL/726843) | 4044181 | 0.10 / 0.025 |
| RC0603FR-071ML | [311-1.00MHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-071ML/726844) | 436984 | 0.10 / 0.025 |

The additional two polymers cost $7.92 at the archived single-unit price.
The 100 nF, 10 uF and 22 uF ceramic donors reuse existing sourced BOM
identities and their existing primary product records above. No fresh stock
or price is invented for those reused donors here. Footprint, courtyard,
polarity, assembly and enclosure-height review remain later PCB work.

## Executable arithmetic

Run these Python blocks independently with Python 3; the second also needs
NumPy. They calculate the stated model and allocations without reading or
writing circuit files. Passing them is arithmetic validation, not a native
connectivity or hardware test.

```python
from math import isclose, log

lo, hi = .999*.9975, 1.001*1.0025
vnom = .795*(1+33000/10000)
vlo = .779*(1+33000*lo/(10000*hi))-100e-9*33000*hi
vhi = .811*(1+33000*hi/(10000*lo))+100e-9*33000*hi
vfull_lo, vfull_hi = vlo-.075, vhi+.075
assert isclose(vnom, 3.4185)
assert isclose(vlo, 3.3284563489051897)
assert isclose(vhi, 3.5094114107340624)
assert isclose(vfull_hi, 3.5844114107340626)
margins = (vfull_lo-3.19395125,
           vfull_lo-.0875-3.1396215743674185, 3.6-vfull_hi)
assert min(margins) > 0
assert isclose(1.8*vfull_hi/.75, 8.60258738576175)
for current in (1.8, 2.25, 3.0):
    pout = current*vfull_hi
    pin = pout/.75
    print('A / output W / input W / input A / loss W',
          current, pout, pin, pin/3.2, pin-pout)

rmin, rmax = .99*.99, 1.01*1.01
por = (1.3-.2)/(110000*rmin)+3.8e-6
en_held_min = 2.7-3.8e-6*110000*rmax
en_source_min = 3.263705830523478-.250-3.8e-6*110000*rmax
run_hi = (3.2-.1-1e-6*1000*rmax)/(1+1000*rmax/(68000*rmin))
run_lo = .1+1e-6*1000*rmax
run_absent = (2e-6+1e-6)*68000*rmax
buffer_dc = 4.6/(68000*rmin)+1e-6
gate_peak = 4*4.6/(1000*rmin)
gate_dc = 4*(4.6/(1e6*rmin)+1e-6)
vgs = (2.6-1e-6*1000*rmax)/(1+1000*rmax/(1e6*rmin))
run_clamped = 4.6*.2/(1000*rmin+.2)
control_spare = .001125-(.000500+.000500+.000004+gate_dc)
assert isclose(por, 14.00304050607081e-6) and por < 15e-6
assert isclose(run_hi, 3.052261794158283)
assert max(run_lo, run_absent, run_clamped) < .3
assert buffer_dc < 100e-6 and gate_dc < 100e-6
assert gate_peak < .050 and vgs > 2.5 and control_spare > 98e-6
print('EN held / source V; RUN high / low / absent / clamped V',
      en_held_min, en_source_min, run_hi, run_lo, run_absent, run_clamped)
print('gate peak A / DC A / VGS V / held spare A',
      gate_peak, gate_dc, vgs, control_spare)

direct, bead, extra_bulk, filters, radio = 147.21, 10, 300, .4, 10.2
assert isclose(direct+bead, 157.21)
assert isclose(direct+bead+extra_bulk+filters, 457.61)
assert isclose(direct+bead+extra_bulk+filters+radio, 467.81)
cpartial_lo = 110.21*.9*.85*.6+47*.8+300*.8*.8
cpartial_hi = 120.41*1.1*1.15+47*1.2+300*1.2*1.7
assert isclose(cpartial_lo, 280.18639)
assert isclose(cpartial_hi, 820.71865)
assert isclose(32*.9*.85*.6, 14.688)
frequencies = [100e6/(8+1.2*162*x) for x in (1, rmax, rmin)]
assert isclose(frequencies[0], 494071.14624505927)
print('RT nominal / resistor-screen Hz', frequencies)
for v in (vhi, vfull_hi):
    il = 3*v/(3.2*.75)
    ripple = 3.2/(2e-6)*(v-3.2)/v/400e3
    print('V / input A / ripple A pp / peak A / DCR-only W',
          v, il, ripple, il+ripple/2, (il*il+ripple*ripple/12)*.018)
    print('850uF energy mJ', .5*850e-6*v*v*1000)
assert isclose(.5*850e-6*vhi*vhi*1000, 5.234286591160937)
print('illustrative linear 6ms capacitor A', 850e-6*vhi/.006)
rbleed_min = 22*rmin/2
rbleed_max = 22*rmax/2+.2
key_return = .0015
hold_charge = 291.6e-6*(3.013705830523478-2.7)-1e-6
held_existing = hold_charge/.001942
held_efuse_proposed = hold_charge/.002092  # Separate, unadopted MR/FLT load.
assert isclose(held_existing, .04658940277067255)
assert isclose(held_efuse_proposed, .043248862419046886)
for cap, expected in ((850e-6, .03464783904068027),
                      (.001, .03899745769491797)):
    off = .010+rbleed_max*cap*log(
        (3.6-rbleed_max*key_return)/(.3-rbleed_max*key_return))
    assert isclose(off, expected)
    assert off < held_efuse_proposed < held_existing
    print('C uF / key-return off ms / existing and proposed hold margins ms',
          cap*1e6, off*1000, (held_existing-off)*1000,
          (held_efuse_proposed-off)*1000)
fall_current = 1.8+vfull_hi/rbleed_min
for cap in (250e-6, 850e-6, .001):
    gradient = cap/fall_current
    assert gradient > 8.4e-6 and gradient < .001
    print('C uF / loaded and bleeder-only fall ms/V',
          cap*1e6, gradient*1000, cap*rbleed_min/vfull_hi*1000)
assert 250e-6*rbleed_min/vfull_hi < .001
assert isclose(fall_current*.001*1e6, 2132.471771037655)
assert isclose(fall_current*8.4e-6*1e6, 17.9127628767163)
startup_net_min, startup_net_max = 850e-6/.020, 250e-6/8.4e-6
assert isclose(startup_net_min, .0425)
assert isclose(2.25+startup_net_min, 2.2925)
assert isclose(250e-6/.001, .250)
print('startup net minimum / maximum A', startup_net_min, startup_net_max)
print('VBATT disabled profile remains conditional on unimplemented invariant.')
print('PWR-004 arithmetic PASS; allocations retain qualification conditions.')
```

```python
import itertools
import math
import numpy as np

vmin, vmax = 3.3284563489051897, 3.5094114107340624
freq = np.logspace(0, 6, 16000)
s = 2j*np.pi*freq
gm, rea, lind = 120e-6, 5e6, 4.4e-6
crossings, phase, peak = [], [], []
cases = 0
for c, vin, v, i, kind, rf, cf, gf in itertools.product(
        [250e-6, 457.21e-6, 850e-6], [3.2, 3.35, 3.45, 3.6, 4.6],
        [vmin, vmax], [.05, 1.8, 3.0], [1, 0, -1],
        [.999*.9975, 1.001*1.0025], [.95*.997, 1.05*1.003], [.8, 1.2]):
    rz, cp = 42200*rf, 4.7e-9*cf
    r = v/i
    yload = kind/r
    gain = 10.8*(vin/v*.75 if vin < v else 1)*gf
    beta = .795/v
    wrhp = vin*vin*r/(v*v*lind) if vin < v else math.inf
    a = (rea+rz)*cp
    zcomp = rea*(1+s*rz*cp)/(1+s*a)
    loop = gm*zcomp*gain*beta/(yload+s*c)*(1-s/wrhp)
    ix = np.where((abs(loop[:-1]) >= 1) & (abs(loop[1:]) < 1))[0]
    assert len(ix) == 1
    n = ix[0]
    crossings.append(freq[n])
    phase.append(180+np.angle(loop[n], deg=True))
    # Closed-loop load-disturbance impedance:
    # Z(s) = (1+a*s)/(d0+d1*s+d2*s*s).
    k = gm*rea*gain*beta
    d0 = yload+k
    d1 = c+yload*a+k*(rz*cp-1/wrhp)
    d2 = c*a-k*rz*cp/wrhp
    poles = np.roots([d2, d1, d0])
    assert max(poles.real) < 0
    t = np.r_[0, np.logspace(-8, -1, 7000)]
    response = np.full(t.shape, 1/d0, dtype=complex)
    for p in poles:
        response += (1+a*p)/(p*(2*d2*p+d1))*np.exp(p*t)
    peak.append(.5*max(abs(response.real)))
    cases += 1
ceiling = 4.7e-6*100e3/(10*lind)
assert cases == 2160
assert max(crossings) < ceiling and min(phase) > 50 and max(peak) < .067
assert math.isclose(max(peak), .06611221222872946)
print('PWR-004 MODEL cases/fc_min/fc_max/PM_min/0.5A_peak',
      cases, min(crossings), max(crossings), min(phase), max(peak))
print('MODEL bandwidth ceiling Hz / remaining combined allowance V',
      ceiling, .075-max(peak))
```

## Completion and qualification boundaries

Before declaring the native migration complete, preserve the verified
RUN buffer/clamp, R34 change, pin mapping and hierarchy connections through
final native review, resolve #846 and update dependent voltage/source/
capacitance records, and inspect fresh native ERC, exports and BOM. Keep
unresolved whole-source circuitry and the independent eFuse proposal visible.

The later PCB/prototype qualification must establish relevant-frequency
capacitance, L under DC bias, loop gain/phase and line/load response,
capacitor ripple and all component temperatures, source impedance/collapse,
autonomous loaded cold/warm startup and reset paths, raw/held RUN sequencing,
and hard-off discharge including every subsequently added load. Constrain
normal operating modes to the completed source budget. Full-temperature
leakage, control timing, supply gradients and backup-supply policy retain
their explicit open allocations until verified or conservatively redesigned.
These are release conditions; unbuilt-board measurements are not prerequisites
to drawing this calculated prototype schematic.
