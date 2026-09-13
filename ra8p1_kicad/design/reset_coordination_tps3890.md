# RST-002: TPS3890 DC and startup reset coordination proposal

Revision 6, 2026-09-12. **MCU native wiring completed and targeted
connectivity checked. Radio switch migration implemented with targeted
native mapping, final page-9 and BOM review passed; DC qualification remains open.** Tracking:
[#846](https://github.com/bsikar/ra8-firmware/issues/846),
[#825](https://github.com/bsikar/ra8-firmware/issues/825),
[#826](https://github.com/bsikar/ra8-firmware/issues/826) and
[epic #821](https://github.com/bsikar/ra8-firmware/issues/821).
The native MCU circuit now contains U2 TPS389001DSET, R67/R74 33k/20k,
R75 10k MR pull-up and C95 10n CT with an RST-002 annotation. Radio
R12/R13 also have the new exact 33k/20k selections. This is a supporting
engineering contract with a bounded native connectivity check, not a completed
reset system, independent whole-circuit validation, qualified hardware or
fabrication approval.

Scope is DC release headroom and conditional startup-delay coordination with
[PWR-006](main_regulator_tps63806.md): main load pins
3.151819680..3.393012496 V, and radio minimum 3.131819680 V after an
allocated 20 mV complete switch-path loss. That loss remains a separate
radio-switch acceptance condition, including the new divider load.
The former U2 release maximum 3.193951250 V and U6 release screen
3.139621574 V cannot simply carry forward to these minimum rails.

Native U4 is now TPS22964CYZPT, with R7 4.7k/R8 10k and C48 removed,
under [RADIO-019](radio_interface.md#radio-019-tps22964c-low-loss-switch-migration).
The 2026-09-12 working XML export confirms all six U4 balls and the exact
ON partition. Native and CLI ERC preserve all 141 baseline findings exactly
(139 errors/two warnings), with the four ignored checks unchanged. The
12-page PDF was visually reviewed before the later page-9 wording fix;
the full PDF is re-exported and final revised page-9 review passed.
Native BOM has 19 columns/84 groups: all 210 included references, values
and MPNs match XML, with no duplicates and only TP1-TP3 excluded.
TPS22997 is not adopted. At a 505 mA path screen, a 30 milliohm installed switch allocation
leaves 9.603960 milliohm for remaining series paths within 20 mV total.
These are qualification allocations, not interpolated datasheet guarantees.
Conditional radio release headroom remains 18.540692 mV; native substitution
does not establish complete path acceptance. No AON_HOLD load is added.

For historical comparison, former TPS22917's 0.5 A * 0.175 ohm = 87.5 mV
loss screen gives radio minimum 3.064319680 V and -48.959308 mV release
headroom. Its failure motivated the replacement; it is not the present
switch's loss model. Preserve it below as historical arithmetic only.

No fast-collapse solution is adopted here. In particular, the ten-IC
[brownout fallback](main_rail_brownout_tps63806.md) is NOT adopted, and its
low-threshold release-supervisor choices must not be separated from its
proposed fast-good startup interlock. This record instead puts the minimum
release threshold above 3 V without that interlock. It does not establish
reset assertion or SDRAM bus quiescence before a fast fall through 3 V.

## Component and native pin contracts

U2 TPS3808G33DBVR has been replaced with **TPS389001DSET**; U6 TPS389001DSET is retained.
This replaces one IC and adds no comparator, reference, latch or one-shot IC.
Native wiring uses R67 upper sense, R74 lower sense, R75 MR pull-up and
C95 CT for U2. The final targeted netlist check confirms the pin partitions
below; BOM and PDF are refreshed. These checks do not independently validate
the entire circuit or qualify its physical behavior.
U2's new DSE pinout is NOT the old DBV pinout. Verify symbol, exact package
and land pattern independently; do not inherit the old footprint.

Primary mapping: [TI TPS3890 SLVSD65A, section 6](https://www.ti.com/lit/ds/symlink/tps3890.pdf).

| TPS389001DSET pin | U2 MCU connection contract | U6 radio connection contract |
| --- | --- | --- |
| 1 SENSE | R67 33k / R74 20k divider junction sensing +3V3_MCU | R12/R13 junction sensing +3V3_RADIO |
| 2 GND | GND | GND, unchanged |
| 3 MR | SW_RESET_N/TP1 request; R75 10k pull-up to +3V3_MCU | RADIO_MR_N from U7.4, unchanged |
| 4 VDD | +3V3_MCU; retain C44 100n bypass | +3V3_MCU; retain C51 100n bypass |
| 5 CT | C95 10n C0G to GND; remove former intentional CT no-connect | Retain C52 1n to GND |
| 6 RESET | MCU_RESET_N; retain R1 pull-up, U7.3 and added microSD U19.6 input | C6_EN/U3.3; retain R11 pull-up to +3V3_RADIO |

No connection from MR to RESET is allowed: that would create a self-held
reset loop. Preserve U7 SN74LVC1G97DBVR, C53, R14 and its existing AND
arbitration of MCU_RESET_N and RADIO_RESET_REQ_N. No open-drain clamp is
added against U7's push-pull output. Retain the separate debugger connection
to MCU_RESET_N and the manual request to MR; external drive must obey each
net's power-domain and drive-type contract. Do not add capacitance on either
protected reset output. Keep U16/Q3, R34, KILL and held discharge unchanged.

### Native checkpoint evidence

The 2026-09-08 final netlist check confirms:

| Net / local partition | Verified endpoints relevant to this migration |
| --- | --- |
| +3V3_MCU | U2.4, R67.1, R75.1 |
| GND | U2.2, R74.2, C95.2 |
| U2 SENSE, exactly this local partition | U2.1, R67.2, R74.1 |
| U2 CT, exactly this local partition | U2.5, C95.1 |
| SW_RESET_N | U2.3, R75.2, TP1.1 |
| MCU_RESET_N | U2.6, J1.10, R1.2, U1.D5, U15.A4, U7.3 |

The later 2026-09-12 microSD working export adds U19.6 to MCU_RESET_N:
the exact seven endpoints are U2.6, J1.10, R1.2, U1.D5, U15.A4, U7.3
and U19.6. This is a gate input, with no MR pullup or AON_HOLD load.
The following ERC/BOM/PDF evidence belongs to the earlier checkpoint;
the current microSD validation is recorded in
[its integration contract](microsd_power_interface.md).

Native and CLI ERC both report 139 errors and two warnings, unchanged from
the preceding checkpoint, with no U2 violations. No rules or exclusions
were changed. This is not a clean ERC result. The native BOM is refreshed;
the full schematic PDF has 12 A3 pages, and changed pages 05/06 were visually
inspected with no annotation overlap found. C44's description now refers
to RST-002; the MCU RST-002 and radio RST-002/PWR-006 notes are saved.
These are targeted connectivity, export and presentation checks, not an
independent whole-circuit validation or hardware qualification.

| Function / references | Exact proposed part | Electrical basis |
| --- | --- | --- |
| Upper sense legs R67 / R12 | Panasonic ERA-6ARW333V | 33.0k, +/-0.05%, +/-10 ppm/C, 0805, 0.125 W |
| Lower sense legs R74 / R13 | Susumu RG1608N-203-W-T1 | 20.0k, +/-0.05%, +/-10 ppm/C, 0603, 0.1 W |
| U2 CT capacitor C95 | TDK C1608C0G1H103J080AA | 10 nF, +/-5%, C0G +/-30 ppm/C, 50 V, 0603 |
| Retained U6 CT, C52 | TDK C1608NP01H102J080AA | 1 nF, +/-5%, NP0 +/-30 ppm/C, 50 V, 0603 |
| U2 MR pull-up R75 | YAGEO RC0603FR-0710KL | Reuse existing 10k MPN, +/-1%, +/-100 ppm/C, 0603 |

Primary component sources: [Panasonic exact 33k model](https://industrial.panasonic.com/jp/products/pt/high-precision-chip-resistors/models/ERA6ARW333V),
[Susumu RG coding/electrical/reliability catalog](https://www.susumu.co.jp/common/pdf/n_catalog_partition01_en.pdf),
[TDK 10n](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1608C0G1H103J080AA),
[TDK retained 1n](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1608NP01H102J080AA),
and [YAGEO exact 10k specification](https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-0710KL).

## Threshold calculation and allocations

TI Table 5 gives nominal VITN=1.15 V and VITP=1.157 V for TPS389001.
Section 7.5 specifies +/-1% accuracy for BOTH thresholds over -40..125 C
and VDD=1.5..5.5 V. Apply the positive-threshold limit directly to VITP;
do not add the maximum hysteresis again to that positive-threshold limit.
The separately specified hysteresis range is 0.325..0.825%.
[TI primary threshold specification](https://www.ti.com/lit/ds/symlink/tps3890.pdf)

Each divider resistor independently receives initial +/-0.05%, its 10 ppm/C
limit over a 100 C excursion from reference temperature, and one additional
allocated +/-0.15% combined assembly/aging term. This is not a guaranteed
simultaneous unlimited-life bound; actual installed resistance must remain
inside the allocated endpoints. Do not claim matched tracking between the
separate manufacturers. The excursion must include actual self-heating and
remain inside both parts' operating/power-derating limits.

Allocate +/-150 nA total adverse SENSE current: the TI 100 nA test-point
limit plus 50 nA board allowance. The silicon limit is listed at VSENSE=5 V;
extending that bound to the near-threshold installed operating condition
is an explicit qualification assumption, not a new guaranteed test condition.
The model uses maximum upper resistance for the adverse voltage term at
both endpoints, conservatively relaxing correlation on the lower endpoint.

```text
fmin = .9995*.999*.9985 = 0.99700274925
fmax = 1.0005*1.001*1.0015 = 1.00300275075
Kmin = 1 + 33000*fmin/(20000*fmax)
Kmax = 1 + 33000*fmax/(20000*fmin)
E = 150nA*33000*fmax
Vfall = 1.15*.99*Kmin-E .. 1.15*1.01*Kmax+E
Vrise = 1.157*.99*Kmin-E .. 1.157*1.01*Kmax+E
```

| Conditional result, either supervisor | Value |
| --- | ---: |
| Falling threshold | 3.000822727..3.094473285 V |
| Rising threshold / start of release timing | 3.019118825..3.113278988 V |
| Minimum main-rail release headroom | 38.540692 mV |
| Minimum radio-rail release headroom after 20 mV path loss | 18.540692 mV |
| Minimum rising threshold above 3 V | 19.118825 mV |
| Divider current upper screen at full maximum rail | 64.211562 uA per divider |

For comparison only, applying the older conservative maximum-hysteresis
construction to the falling threshold gives 3.119961730 V maximum release,
still leaving 31.857950/11.857950 mV main/radio headroom. The direct VITP
calculation above is supported independently by the primary specification.
The margins are not additional regulator transient allocations: PWR-006's
single 75 mV disturbance allocation is already inside the supplied rail bounds.

## Release timing: separate from falling propagation

Use TI's CT charge limits VCT=1.17..1.29 V and ICT=0.90..1.35 uA.
Allocate 10 nA additional external CT-node leakage in either direction;
this covers capacitor/board loading, not another internal-current tolerance.
Assume CT starts discharged. For both C0G parts, use initial +/-5% and
30 ppm/C over 100 C, without an invented X7R DC-bias factor.
[TI sections 7.5-7.6 and 8.3.1](https://www.ti.com/lit/ds/symlink/tps3890.pdf)

```text
Cmin = Cnom*.95*.997
Cmax = Cnom*1.05*1.003
t_charge_min = Cmin*1.17/(1.35uA+10nA)
t_charge_max = Cmax*1.29/(.90uA-10nA)
```

| Channel / CT | Nominal total using typical 25us term | Charge-only minimum | Charge-only maximum |
| --- | ---: | ---: | ---: |
| MCU / 10n | 10.720652 ms | 8.148276 ms | 15.264758 ms |
| Radio / retained 1n | 1.094565 ms | 0.814828 ms | 1.526476 ms |

The MCU charge-only minimum exceeds the documented 2.4 ms power-on RES
minimum by 3.395115 times. Radio's minimum exceeds 50 us by 16.296551 times.
These comparisons support conditional startup component selection; they do
not prove that the supply has settled before release. Sources:
[RA8P1 datasheet reset timing](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet),
[RST-001 existing requirement basis](boot_and_debug.md#rst-001-external-supervisor-implementation-basis),
and [Espressif power-up/reset guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c6/schematic-checklist.html#chip-power-up-and-reset-timing).

The charge-only maxima are NOT total maximum reset delays. TI lists 25 us
open-CT release delay at 3.3 V and 325 us startup delay as nominal, not
maxima. Its falling SENSE delay is 18 us nominal at 3.3 V and 5% overdrive,
without a maximum. MR requires a minimum 1 us request pulse; its 250 ns
response entry is nominal. Do not use these typical values as worst-case
fast-fault response limits. Residual CT charge, repeated dips, low-POR ramps
and actual protected-pin low duration retain qualification requirements.

## MR, output loading and power

Reuse RC0603FR-0710KL for the new U2 MR pull-up. The existing 1% initial
and 100 C/100 ppm/C screen gives 9801..10201 ohm. Allocate at most 50 uA
adverse total MR-node current in either direction, including supervisor,
fixture and board. TI does not provide a separate guaranteed MR current
maximum here; the allocation must be established in the installed circuit.
This screen applies while main is in the stated valid rail range.

```text
MR_high_min = 3.151819680 - 50uA*10201 = 2.641769680 V
MR_high_margin = MR_high_min - .7*3.151819680 = 0.435495904 V
Manual-driver sink <= 3.393012496/9801 + 50uA = 0.396190439 mA
MR pull-up power <= 3.393012496^2/9801 = 1.174628487 mW
```

Require the manual request driver to sink that current with VOL <=0.25*VDD
and to be high impedance when released/unpowered; no precision pull-up MPN
is necessary. U7's radio MR path retains its separate <=100 uA output-load
allocation and logic-level checks from [RADIO-015](radio_interface.md#radio-015-reset-request-arbitration).
This does not put a 10k pull-up on U7's output or silently change its load.

Retain [RADIO-013](radio_interface.md#radio-013-reset-output-and-module-enable-pull-up)
and R1's protected-output loading reviews. U6's separate radio output retains
its 12 uA allocation and 0.358190439 mA screen at maximum main rail.
For U2, CMS-011C's former 12 uA device screen explicitly comprised MCU 5 uA,
U7 5 uA and flash 2 uA; it did not reserve microSD U19. Add U19's 5 uA
input allocation, giving 17 uA and 0.363190439 mA. Preserve CMS-011C's
13 uA additional allowance by increasing its 25 uA total to **30 uA**.
This total must include supervisor released-output leakage, board, probe
and any other adverse leakage; it is an acceptance allocation, not a
measured or universally guaranteed current sum.

With R1=9801..10201 ohm, use the total in either adverse direction:

```text
U2 sink <= 3.393012496/9801 + 30uA = 0.376190439 mA <0.4 mA
Released high >= 3.151819680 - 30uA*10201 = 2.845789680 V
Margin to MCU 0.8*VCC = 0.324333936 V at minimum normal rail
At falling-corner rail 3.000822727 V: high >=2.694792727 V
Margin to MCU 0.8*VCC at that corner = 0.294134545 V
Added U19 drop / margin reduction = 5uA*10201 = 0.051005 V
```

The MCU RES Schmitt row is identified in [boot/debug](boot_and_debug.md).
The falling-corner calculation is a static released-node screen before
assertion, not a propagation or rail-collapse guarantee. Flash RESET#
threshold/loading and debugger behavior retain their receiver checks.
At the separate 3.0 V gate test point, the 30 uA model gives 2.693970 V,
0.823970 V above U7/U19's 1.87 V maximum positive-going threshold.
This point does not establish a continuous-rail threshold bound. TI's
5 uA input-current limit uses VI=5.5 V or GND; Ioff has a separate 10 uA
limit at VCC=0. The 3.5 pF input capacitance is typical, not a maximum.
Do not extend those test conditions through arbitrary partial power or
claim a guaranteed reset rise time. [TI SN74LVC1G97 SCES416N, section 6.5](https://www.ti.com/lit/ds/symlink/sn74lvc1g97.pdf).

U2's VOL<=0.25 V at VDD>=1.5 V/0.4 mA supports the conditional sink screen;
its 250 nA released-output leakage test-point limit fits within, but does
not validate, the 13 uA remainder. Radio EN's 3.3 V/25 C table is not a
full-temperature guarantee. Do not infer a guaranteed low-POR reset level
from the 15 uA POR test with an arbitrarily stronger pull-up. The new U19
input adds no intentional capacitor; qualify total fanout, release edges,
startup and collapse with the installed circuitry and permitted probe.

Both supervisor VDD pins remain on +3V3_MCU. TI supply-current maxima are
5.8 uA at 3.3 V and 6.5 uA at 5.5 V over temperature; typical is 2.09 uA
at 3.3 V. These are test-point limits, not a guaranteed continuous-VDD
interpolation. Two maximum-screen divider currents total 128.423124 uA.
Include supervisor, MR and asserted-output currents in the existing 1.8 A
system allocation; do not confuse them with the fallback's 25 mA budget.
No load is added to AON_HOLD. The new 10n CT storage is private timing-node
storage, not instantaneous main-rail parallel decoupling; count its charge
separately when refreshing startup/discharge accounting. Reused bypasses
and retained C52 are not newly added rail capacitance.

## Explicit remaining boundaries

- Native U2 replacement and R67/R74/R75/C95 wiring are completed and the
  targeted pin partitions checked as recorded above. Full-system radio DC,
  startup and fault qualification remain unresolved; the existing ERC count
  is not a clean schematic or fabrication-release result.
- The minimum falling threshold is only 0.822727 mV above 3 V. There is
  no worst-case propagation proof before a falling rail crosses 3 V.
- RA8P1 general VCC/VCC2 operation extends below 3 V; its SDRAM controller
  timing, the selected SDRAM and the radio impose separate 3 V system
  operating constraints. Reset assertion alone does not prove cessation
  of SDRAM transactions, retention or corruption-free power failure.
- PWR-004's VBATT-disabled backup-switch firmware invariant is unchanged,
  still an unimplemented source-based inference/contract, not achieved by
  replacing the external reset IC. Preserve supply-gradient qualification.
- Loaded startup/settling, controlled shutdown, bounded fault response,
  radio off-state leakage and reset-to-memory-quiescence remain separate
  engineering decisions and qualification work. This proposal does not
  assume universal-short protection or adopt the oversized fallback.

## Procurement checkpoint

Read 2026-09-08. USD cut-tape unit prices at quantities 1/10/100; stock is
unreserved and can change. Shipping, tax, reeling and tariff are excluded.
Retained C52 and bypass parts retain their prior sourcing records rather
than implying a new purchase. New-MPN footprints require explicit selection.

| Exact part | DigiKey cut-tape part number / direct listing | Stock | USD 1 / 10 / 100 |
| --- | --- | ---: | --- |
| TPS389001DSET | [296-44489-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/TPS389001DSET/6110554) | 2090 | 2.22 / 1.643 / 1.3395 |
| ERA-6ARW333V | [P33KBSCT-ND](https://www.digikey.com/en/products/detail/panasonic-industry/ERA-6ARW333V/3073417) | 9330 | 0.57 / 0.472 / 0.391 |
| RG1608N-203-W-T1 | [RG16N20.0KWCT-ND](https://www.digikey.com/en/products/detail/susumu/RG1608N-203-W-T1/600628) | 78737 | 0.64 / 0.529 / 0.4381 |
| C1608C0G1H103J080AA | [445-7404-1-ND](https://www.digikey.com/en/products/detail/tdk-corporation/C1608C0G1H103J080AA/2732839) | 24965 | 0.24 / 0.141 / 0.0886 |
| RC0603FR-0710KL | [311-10.0KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/726880) | 2797669 | 0.10 / 0.025 / 0.0122 |

## Saved native annotation references

These UUIDs identify the actual saved text objects, not proposed replacement
text. The file links locate their native sheets; the UUIDs are stable object
identities, not line-number citations.

| Native sheet | Note title | Text-object UUID |
| --- | --- | --- |
| [MCU clocks/debug](../ereader/mcu_clocks_debug.kicad_sch) | RST-002: U2 TPS389001DSET | `46f0ad44-622c-44b3-b5a3-915f3fac5f52` |
| [Radio](../ereader/radio_esp32.kicad_sch) | RADIO-014 / RST-002: R12=33k, R13=20k | `c87b6eaf-8d4f-4da0-88a2-efff6aae0d55` |
| [Radio](../ereader/radio_esp32.kicad_sch) | PWR-006: upstream U13 TPS63806YFFR | `1e311763-baef-42a6-83aa-c305d9e30b3d` |

The MCU note exposes divider, threshold, CT and MR calculations. Both radio
notes described the preceding TPS22917 checkpoint. RADIO-019 now records
the targeted working-netlist evidence for the new switch and current
migration validation status. Native/CLI ERC findings are unchanged; the
final revised page-9 visual review and full BOM reference/value/MPN
reconciliation passed. These are not radio electrical qualification.

## Reproducible arithmetic

Run with Python 3. This checks the proposed inputs only, not native circuitry.

```sh
python3 - <<'PY'
from math import isclose, prod

fmin, fmax = (prod(1+s*x for x in (.0005, .001, .0015)) for s in (-1, 1))
rt, rb = 33000, 20000
kmin, kmax = 1+rt*fmin/(rb*fmax), 1+rt*fmax/(rb*fmin)
error = 150e-9*rt*fmax
fall = (1.15*.99*kmin-error, 1.15*1.01*kmax+error)
rise = (1.157*.99*kmin-error, 1.157*1.01*kmax+error)
legacy_hi = 1.15*1.01*1.00825*kmax+error
main_min, rail_max = 3.151819680, 3.393012496
radio_min = main_min-.020
old_radio_min = main_min-.5*.175
old_radio_margin = old_radio_min-rise[1]
assert isclose(old_radio_min, 3.064319680, abs_tol=1e-12)
assert isclose(old_radio_margin, -.0489593084817544, abs_tol=1e-12)
assert old_radio_margin < 0
divider_max = rail_max/((rt+rb)*fmin)
expected = (3.000822726706337, 3.0944732850469583,
            3.019118825082214, 3.1132789884817544)
for actual, target in zip(fall+rise, expected):
    assert isclose(actual, target, rel_tol=1e-12, abs_tol=1e-12)
assert 3 < rise[0] < rise[1] < radio_min < main_min
assert isclose(legacy_hi, 3.1199617295237623, abs_tol=1e-12)
assert isclose(divider_max, 64.21156185002685e-6, abs_tol=1e-15)
print('RST-002 falling/rising V:', fall, rise)
print('Main/radio release margins V:', main_min-rise[1], radio_min-rise[1])
print('Historical TPS22917 old-screen radio minimum/margin V:',
      old_radio_min, old_radio_margin)
print('Legacy conservative high V and margins:', legacy_hi,
      main_min-legacy_hi, radio_min-legacy_hi)
print('Divider upper current per/two A:', divider_max, 2*divider_max)

for cap, required in ((1e-9, 50e-6), (10e-9, 2.4e-3)):
    cmin, cmax = cap*.95*.997, cap*1.05*1.003
    tmin = cmin*1.17/(1.35e-6+10e-9)
    tmax = cmax*1.29/(.90e-6-10e-9)
    nominal = cap*1.23/1.15e-6+25e-6
    assert 0 < required < tmin < nominal < tmax
    assert isclose(tmin/cap, 814827.5735294117, rel_tol=1e-12)
    print('CT F, nominal/charge-only min/max s, minimum ratio:',
          cap, nominal, tmin, tmax, tmin/required)

rmin, rmax = 10000*.99*.99, 10000*1.01*1.01
mr_high = main_min-50e-6*rmax
mr_margin = mr_high-.7*main_min
mr_sink = rail_max/rmin+50e-6
radio_reset_sink = rail_max/rmin+12e-6
reset_sink = rail_max/rmin+17e-6
reset_total_sink = rail_max/rmin+30e-6
reset_high = main_min-30e-6*rmax
reset_high_margin = reset_high-.8*main_min
reset_fall_high = fall[0]-30e-6*rmax
reset_fall_margin = reset_fall_high-.8*fall[0]
gate_3v_margin = 3-30e-6*rmax-1.87
pull_power = rail_max**2/rmin
assert isclose(mr_high, 2.641769680, abs_tol=1e-12)
assert isclose(mr_margin, .435495904, abs_tol=1e-12) and mr_margin > 0
assert max(mr_sink, radio_reset_sink, reset_sink, reset_total_sink) < .0004
for actual, target in ((reset_sink, .0003631904393429242),
                       (reset_total_sink, .0003761904393429242),
                       (reset_high, 2.845789680),
                       (reset_high_margin, .324333936),
                       (reset_fall_high, 2.6947927267063374),
                       (reset_fall_margin, .2941345453412675),
                       (gate_3v_margin, .823970),
                       (5e-6*rmax, .051005)):
    assert isclose(actual, target, abs_tol=1e-12)
assert isclose(pull_power, .0011746284866862718, abs_tol=1e-15)
print('MR high/margin V, manual/reset sink A, pull-up W:',
      mr_high, mr_margin, mr_sink, reset_sink, pull_power)
print('Reset 30uA total sink A; released high/margin V:',
      reset_total_sink, reset_high, reset_high_margin)
print('Reset falling-corner high/margin V; gate 3V-point margin V:',
      reset_fall_high, reset_fall_margin, gate_3v_margin)
print('RST-002 arithmetic PASS; targeted native connectivity checked; radio DC unresolved; no fast-fault proof.')
PY
```
