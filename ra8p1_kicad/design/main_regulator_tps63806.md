# PWR-006: TPS63806 main-regulator replacement design basis

Revision 1 draft, 2026-09-08. Tracking: [power #825](https://github.com/bsikar/ra8-firmware/issues/825),
[architecture #823](https://github.com/bsikar/ra8-firmware/issues/823), and
[SDRAM voltage compatibility #846](https://github.com/bsikar/ra8-firmware/issues/846).

This is a calculated replacement proposal for U13, not a statement that
TPS63806YFFR or the divider below has been adopted in the native schematic.
The inspected BOM still identifies U13 as LTC3119IUFD#PBF, R41 as 33 kOhm,
R42 as 10 kOhm and L2 as 3.3 uH. The saved implementation is described by
[PWR-004](main_regulator_ltc3119.md). That record's voltage, compensation and
startup calculations do not transfer to TPS63806. Native migration, exact
new MLCC/inductor selection, reset coordination and qualification remain
separate acceptance steps. No fabrication approval is implied.

### Library-preparation checkpoint

The project-local `Power_Devices:TPS63806YFFR` symbol is now saved in
[Power_Devices.kicad_sym](../libs/symbols/Power_Devices.kicad_sym). It has
10 pin groups representing all 15 physical balls, all visible, with 200 mil
pin lengths, endpoints on a 100 mil grid and 50 mil pin text. The native
symbol checker reports no issues. This is library preparation only:
schematic U13 remains LTC3119IUFD#PBF and no TPS63806 appears in the BOM.
It is not a circuit-level connectivity or ERC qualification.

The symbol's footprint is deliberately blank. Selection and verification
of the exact YFF 15-ball WCSP land pattern, ball numbering, package revision
and assembly capability are deferred. A blank footprint is an explicit
layout-release blocker, not permission to reuse the LTC3119 QFN footprint.

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
excluded. Library availability does not mean schematic adoption.

| Proposed role | Exact component | Nominal / initial tolerance / TCR |
| --- | --- | --- |
| R41 replacement, output sense to FB | Susumu RG2012V-562-P-T1 | 5600 ohm / +/-0.02% / +/-5 ppm/C; 0805 |
| R42 replacement, FB to lower-series node | Susumu RG2012L-102-L-T05 | 1000 ohm / +/-0.01% / +/-2 ppm/C; 0805 |
| New lower-series trim resistor, node to quiet ground | YAGEO RC0603FR-0710RL | 10 ohm / +/-1% / +/-200 ppm/C; 0603; reference unassigned |

The 10 ohm part is in series with the 1000 ohm lower leg, not parallel
with it or in series with FB. The resulting 1010 ohm nominal lower leg
is below TI's 100 kOhm maximum. No feedforward capacitor is selected.
The 0805 precision parts require intentional footprint changes from the
existing divider; an MPN-only substitution is insufficient.

Sources: [Susumu RG catalog, electrical and reliability tables](https://www.susumu.co.jp/common/pdf/n_catalog_partition01_en.pdf),
[Susumu ultra-precision RG specification and order coding](https://www.susumu.co.jp/common/pdf/RG_LL_Data_Sheet.pdf),
and [YAGEO exact 10 ohm specification](https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-0710RL).
These establish the tolerance/TCR selections. Divider procurement
availability and exact packaging are not asserted by this draft.

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
their own coordinated review. Reset component selection is explicitly
pending; do not carry forward PWR-004's reset-margin conclusions.

## Proposed pin mapping and preserved held-enable control

This table is a migration requirement, not a verified native pin audit.
The ball mapping is from TI Table 7-1; confirm symbol and footprint against
the manufacturer's package drawing before replacing U13.

| TPS63806 balls | Proposed connection |
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

TI specifies EN/MODE high >=1.2 V and low <=0.4 V. Recheck the complete
raw/held trajectory with the new input leakage and loading; preserved
wiring does not automatically preserve every prior numerical RUN bound.
The held clamp remains necessary to make shutdown independent of U16's
unspecified behavior below its valid supply range. Keep the existing
10 ms complete response allocation and 1 uC transition-charge reserve.
Do not add the separately proposed eFuse/MR load or change the source-sense
point through this migration. Raw input protection remains unresolved.

## Existing passive inventory and proposed disposition

The following references and exact identities are inherited from PWR-004,
not newly selected TPS63806 passives:

| Existing references | Existing selection | Migration disposition |
| --- | --- | --- |
| L2 | Eaton EXLA1V0703-3R3-R, 3.3 uH | Replace; not compatible with the proposed 0.47 uH filter basis |
| C73 | TDK C3216X7R1V106K160AC, 10 uF | Input-side inventory; reuse or replacement pending effective-capacitance review |
| C96 | Murata GRM32ER71C226KEA8L, 22 uF | Input-side inventory; final population pending |
| C74, C75 | Murata GRM32ER71C226KEA8L, 22 uF each | Existing local output inventory; new MLCC selection pending |
| C99, C100 | Panasonic EEF-JX0J151RF, 150 uF each | Existing output bulk; retention/removal pending startup, loop and storage review |
| C93 | TDK C1608X7R1H104K080AA, 100 nF | Input-side bypass inventory; final population pending |
| C94, C95 | Same TDK 100 nF, LTC bootstrap capacitors | Obsolete functions in proposed TPS63806 circuit; remove during native migration |
| C97 | TDK C3216X7R1V106K160AC, 10 uF, LTC internal VCC | Obsolete function; remove, do not reconnect automatically to output |
| R66, R67, C98 | 162 kOhm RT; 42.2 kOhm VC resistor; 4.7 nF C0G compensation | Obsolete LTC functions; remove during native migration |
| C101 | TDK C1608X7R1H104K080AA, 100 nF | Preserve raw U16 bypass; not output storage |

TI Section 8.3 requires at least 4 uF effective input capacitance and
21 uF effective output capacitance for this voltage, with 0.37..0.57 uH
effective inductance. Section 10.2.2.3 recommends two nominal 47 uF output
ceramics below 3.6 V and states no upper capacitance limit. That does not
establish startup timing, an arbitrary distributed network's phase margin,
or compliance with the product's discharge ceiling. Exact new MLCC and
inductor choices are pending; no datasheet example MPN is adopted here.
The former LTC external-compensation model and 250..850 uF model window
are not a TPS63806 stability proof.

### Unselected local-output MLCC candidate

Three TDK **C3225X7R1C226M250AC** capacitors are a candidate, not a selected
or qualified population. The [exact TDK product record](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C3225X7R1C226M250AC)
lists Production status, 22 uF +/-20%, 16 V and X7R. The
[DigiKey candidate listing](https://www.digikey.com/en/products/detail/tdk/C3225X7R1C226M250AC/1587497)
showed 145 in stock on 2026-09-08, unreserved.

The conditional lower screen is `3*22uF*0.8*0.85*0.6 = 26.928 uF`:
0.8 is initial tolerance, 0.85 is the X7R temperature factor, and 0.6 is
an additional allocated combined bias/aging retention factor. The 0.6 term
is not a guaranteed TDK characteristic. Thus 26.928 uF exceeds the 21 uF
requirement only if that retention allocation and relevant-frequency local
behavior are established for the installed parts. It is not a qualified
minimum. No native capacitor reference or final storage total is changed
by this candidate screen; startup, loop, ripple and discharge review remain
required before adoption. Reset proposals remain pending independent review
and are not incorporated at this library-preparation checkpoint.

### Source-charged storage and shutdown

PWR-004 inventories 147.21 uF directly on the main rail plus C43's 10 uF
behind FB1, before its added C99/C100 300 uF. The four C58-C61 button
filters add 0.40 uF through R27-R30; radio-on adds 10.20 uF separately.
Thus its retained-population nominal examples are 457.61 uF cold/radio-off
and 467.81 uF radio-on. These are traceable starting inventories, not final
TPS63806 capacitance totals. Recompute after every removal/replacement:
`Cnew = Cold - Cremoved + Cadded`, separately for direct, ferrite-isolated,
resistively coupled, switched and input-side storage.

At the proposed full upper voltage, those nominal inventories store
1.552676 / 1.587285 mC and 2.634125 / 2.692839 mJ respectively. The
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
including ripple, temperature and every conversion mode. Final inductor
current, hot DCR and saturation checks are pending the exact part.

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
for capacitance in (457.61e-6, 467.81e-6, 1e-3):
    print('Inventory F / charge C / energy J:', capacitance,
          capacitance*complete[1], .5*capacitance*complete[1]**2)
tail = .010 + 11.4211*.001*log((3.6-11.4211*.0015)/(.3-11.4211*.0015))
assert isclose(tail, .038997458, abs_tol=1e-9)
assert tail < .046589403
print('Conservative 1mF shutdown s:', tail)
print('Illustrative IC thermal W at 85C:', (125-85)/78.8)
candidate_uf = 3*22*.8*.85*.6
assert isclose(candidate_uf, 26.928, abs_tol=1e-12)
assert candidate_uf > 21
print('Unselected MLCC conditional lower screen uF:', candidate_uf)
print('PASS arithmetic only; native migration and qualification pending.')
PY
```

## Acceptance boundary

This basis supports a proposed prototype drawing, not a finished product
voltage contract. Incorporate the independent reset and MLCC selection
results before declaring the migration complete. Then verify the full native
pin/footprint mapping, held-enable hierarchy, exact BOM and refreshed ERC,
netlist and PDF; update dependent records to their actual adopted state.
Bench release additionally requires source protection/budget, all-corners
rail behavior, loaded startup, relevant-frequency PDN/loop response,
temperature and hard-off/backpower qualification. #846 is not closed by
nominal divider arithmetic alone.
