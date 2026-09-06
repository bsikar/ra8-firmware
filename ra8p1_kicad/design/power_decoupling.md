# Power decoupling engineering records

## PWR-001: C39 MIPI analog-supply bypass

Revision 2, 2026-09-05. Applies to C39 and U1 unit M on
[RA8P1 IO allocation](../ereader/mcu_interfaces.kicad_sch), sheet 2 of the
[full schematic PDF](../exports/ereader_rev1.pdf). The PWR-001 schematic
annotation links back here. Tracking: [issue #824](https://github.com/bsikar/ra8-firmware/issues/824).

The [Renesas Quick Design Guide](https://www.renesas.com/en/document/apn/ra8p1-mcu-quick-design-guide),
R01AN7883EU0110, Table 2, p.7, specifies a 100 nF bypass between AVCC_MIPI
and VSS_MIPI. This is a manufacturer-prescribed nominal bypass value, not a
value derived from a measured transient-current waveform. C39.1 connects to
U1.T4 / +3V3_MCU; C39.2 connects to GND, shared with U1.R3 / VSS_MIPI.

MIPI is unused. The [RA8P1 Hardware User's Manual](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
revision 1.30, section 21.4, p.862, requires AVCC_MIPI connected to VCC,
VSS_MIPI to VSS, and VCC18_MIPI plus the six D-PHY lanes left open. The
existing no-connect markers implement that unused-interface treatment.
C39 is not connected to VCC18_MIPI.

### Exact candidate and sourcing

TDK C1608X7R1H104K080AA: 100 nF +/-10%, 50 VDC, X7R, -55 to +125 C,
commercial 0603. TDK's [exact product record](https://product.tdk.com/ja/search/capacitor/ceramic/mlcc/info?part_no=C1608X7R1H104K080AA)
lists production status. Footprint qualification is deferred with the PCB.

[DigiKey 445-1314-1-ND](https://www.digikey.com/en/products/detail/tdk-corporation/C1608X7R1H104K080AA/513811)
snapshot 2026-09-05: Active, 374087 in stock, 24-week standard lead time;
USD 0.11 at quantity 1, 0.06 at 10, 0.0359 at 100. Prices exclude shipping
and taxes. This availability and low unit cost support the initial choice;
neither establishes electrical qualification.

### Arithmetic and limitations

```text
Nominal capacitance = 100 nF
Initial tolerance = +/-10% = +/-10 nF
C_initial_min = 100*(1-0.10) = 90 nF
C_initial_max = 100*(1+0.10) = 110 nF
Nominal voltage utilization = 3.3/50*100 = 6.6%
At a 3.6 V screening voltage: 3.6/50*100 = 7.2%
```

The 3.6 V screening case is not a verified regulator maximum: the source
rail and its tolerance/transients remain to be completed. Voltage utilization
is NOT a capacitance-retention percentage. Initial tolerance excludes DC
bias, temperature dependence, aging, AC excitation and measurement conditions.
X7R temperature classification does not establish DC-bias behavior. No
guaranteed minimum operating capacitance or PDN impedance is claimed here.
Nominal DC-bias screening is recorded below; final rail and PDN requirements
remain to be qualified. Layout must provide a short local
bypass loop between the named supply and ground pins; that is a later PCB task.

### Nominal DC-bias screening and shared bypass selection

TDK's English product page provides a DC Bias Characteristic graph with a
Download data as CSV button. The browser download on 2026-09-05 is preserved
as [manufacturer curve data](../resources/datasheets/TDK_C1608X7R1H104K080AA_dc_bias_2026-09-05.csv),
with only UTF-8 BOM removal, line-ending normalization and trailing blank-line
removal. Numeric values and the manufacturer/date/part headers are unchanged.
The page explicitly identifies these curves as reference data that do not
guarantee product characteristics.

The two bracketing samples are 99.5575 nF at 3.15 V and 98.9525 nF at 4 V.
Linear interpolation, not a new manufacturer measurement, gives:

```text
C(V) = 99.5575 + (V-3.15)/(4-3.15)*(98.9525-99.5575) nF
C(3.3) = 99.450735... nF; nominal loss = 0.549265...%
C(3.6) = 99.237206... nF; nominal loss = 0.762794...%
```

This small nominal loss supports selecting the part for the existing 100 nF
bypass positions. It does not close final rail, temperature/aging, layout or
PDN qualification, and it is not an all-corners capacitance guarantee.
No minimum effective-capacitance requirement has been invented from the
vendor's nominal 100 nF prescription.

The same exact part is selected for C6 and C16-C34, whose current netlist
places pin 1 on +3V3_MCU and pin 2 on GND. Together with C39 these are 21
capacitors. This selection does not apply to the 220 nF VCL bypasses, bulk
input/output capacitors, or either oscillator's load capacitors. Supply
function changes require reevaluation of the affected bypass selection.

### Reproducible Python arithmetic and BOM check

Run from the repository root. This checks only the stated arithmetic and
exact exported BOM inputs; it does not modify the design or simulate a PDN.

```sh
python3 - <<'PY'
import csv
from fractions import Fraction as F

with open('ra8p1_kicad/exports/ereader_rev1_bom.csv', newline='') as stream:
    bom = list(csv.DictReader(stream))
references = {'C6', 'C39'} | {'C'+str(n) for n in range(16, 35)}
for reference in sorted(references):
    rows = [r for r in bom if reference in r['Reference'].split(',')]
    if len(rows) != 1:
        raise ValueError(reference + ' must occur exactly once in BOM')
    row = rows[0]
    if (row['Value'], row['Manufacturer_Part_Number'], row['DNP']) != (
            '100n', 'C1608X7R1H104K080AA', ''):
        raise ValueError(reference + ' calculation inputs differ from BOM')
checks = ((100*(1-F(1, 10)), 90), (100*(1+F(1, 10)), 110),
          (F(33, 10)/50*100, F(33, 5)), (F(36, 10)/50*100, F(36, 5)))
for actual, expected in checks:
    if actual != expected:
        raise ValueError((actual, expected))
print('PWR-001 PASS: 90..110 nF initial; voltage utilization 6.6% / 7.2%.')
curve_path = ('ra8p1_kicad/resources/datasheets/'
              'TDK_C1608X7R1H104K080AA_dc_bias_2026-09-05.csv')
with open(curve_path, newline='', encoding='utf-8-sig') as stream:
    rows = list(csv.reader(stream))
if rows[5] != ['C1608X7R1H104K080AA'] or rows[6] != ['DC/V', 'Capacitance(Nom.)/F']:
    raise ValueError('Unexpected curve identity or units')
samples = {F(r[0]): F(r[1])*10**9 for r in rows[7:] if len(r) == 2}
a, b = F('3.15'), F(4)
if (samples[a], samples[b]) != (F('99.5575'), F('98.9525')):
    raise ValueError('DC-bias source samples changed')
for v, expected in ((F('3.3'), F(676265, 6800)),
                    (F('3.6'), F(674813, 6800))):
    interpolated = samples[a] + (v-a)/(b-a)*(samples[b]-samples[a])
    if interpolated != expected:
        raise ValueError('DC-bias interpolation mismatch')
    print(f'{float(v):.1f} V: {float(interpolated):.6f} nF nominal reference estimate')
print('21 BOM inputs agree. Final rail and PDN qualification remain open.')
PY
```

## PWR-002: Main-rail regulation and reset headroom

Revision 1, 2026-09-05. Linked from the PWR-002 annotation on the
[radio sheet](../ereader/radio_esp32.kicad_sch). Tracking:
[power #825](https://github.com/bsikar/ra8-firmware/issues/825),
[architecture #823](https://github.com/bsikar/ra8-firmware/issues/823), and
[radio #826](https://github.com/bsikar/ra8-firmware/issues/826).
This is a regulator-selection calculation, not an implemented power supply.

### Source limits and operating-mode boundary

[TI TPS63802 SLVSEU9D](https://www.ti.com/lit/ds/symlink/tps63802.pdf),
section 8.5, specifies 500 mV nominal feedback, +/-1% accuracy in PWM mode,
and 100 nA maximum feedback bias at 500 mV. Section 10.2.2.5 limits the
bottom feedback resistor to 100 kohm. The 511k/91k reference divider is
not an exact 3.300 V setting. PG's 95% rising/90% falling thresholds are
typical; PG does not replace a guaranteed reset threshold.

The candidate's +/-1% PWM specification must not be applied to all low-load
PFM behavior. An eventual MODE control must establish PWM before relying
on this calculation during radio operation; transition settling and the
sleep-mode rail envelope require separate verification. No MODE control
is implemented yet. These facts prevent treating the earlier assumed
3.3 V +/-2% screen as a completed regulator specification.

### Divider calculation

Let Rt be VOUT-to-FB resistance and Rb be FB-to-ground resistance. Let Ib
be positive when flowing into FB. KCL gives:

```text
(Vout - Vfb)/Rt = Vfb/Rb + Ib
Vout = Vfb*(1 + Rt/Rb) + Ib*Rt

fmin = (1 - initial_tolerance)*(1 - TCR*100 C)
fmax = (1 + initial_tolerance)*(1 + TCR*100 C)
Vmin = 0.495*(1 + Rt*fmin/(Rb*fmax)) - 100e-9*Rt*fmax
Vmax = 0.505*(1 + Rt*fmax/(Rb*fmin)) + 100e-9*Rt*fmax
```

The symmetric 100 nA term is a conservative screening allocation, including
an adverse polarity; it is not a manufacturer specification for every
unpowered state or PCB contamination condition. The 100 C excursion from
25 C is an allocation for the resistor calculation. Exact resistor parts
are not selected or fitted in either example below.

| Example divider | Assumed initial / TCR | Nominal V | PWM static minimum V | PWM static maximum V |
| --- | --- | --- | --- | --- |
| 511k / 91k | 1% / 100 ppm/C | 3.307692308 | 3.113494435 | 3.508630214 |
| 56k / 10k | 0.1% / 25 ppm/C | 3.300000000 | 3.242044111 | 3.358485094 |

The first example demonstrates why copying reference resistor values and
using ordinary 1% parts cannot justify the existing reset margins. It is
not a claim about the tolerances actually fitted to TI's evaluation board.
The second is the preferred direction for resistor sourcing, at the cost
of higher divider current: 3.3/66000 = 50 uA nominal, versus about 5.495 uA
for the first example. Its 10k bottom resistor satisfies the 100k limit.
This choice still does not approve the whole converter.

### Cross-sheet voltage budget

Using the precision-divider screen, RADIO-004's conditional 0.5 A radio
load and 0.175 ohm switch resistance allowance:

```text
Switch drop = 0.5*0.175 = 0.0875 V
Radio minimum, static screen = 3.242044111 - 0.0875 = 3.154544111 V
U6 maximum rising screen, RADIO-011 example = 3.132066484 V
Remaining radio release budget = 22.477627 mV
U2 maximum rising screen, RST-001 = 3.193951250 V
Remaining MCU release budget = 48.092861 mV
Headroom below 3.6 V = 3.6 - 3.358485094 = 241.514906 mV
```

These are remaining allocations, not measured ripple or guaranteed transient
margins. Routing loss, regulator transients, load steps, startup, operating
mode and any unaccounted static error must fit their respective budgets.
The narrow radio budget makes reducing load-switch drop worth comparing
before approving the supervisor divider. Do not automatically raise the
main rail: all connected devices and reset thresholds need reevaluation.

### Current accounting boundary

[RA8P1 datasheet Rev.1.30](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet),
Table 2.8, p.59, gives ICC = 6.27 mA maximum and ICC_DCDC = 390 mA in
the 1 GHz/250 MHz, 95 C, 3.3 V maximum-condition row. Its note 4 uses
typical DCDC efficiency for ICC_DCDC, so 396.27 mA is a reference screen,
not a guaranteed all-corners input-current bound. IDD is internal core
current; do not add its 1000 mA limit again to the 3.3 V source load.
The table excludes output-pin loading and BGO operation.

[ESP32-C6-WROOM-1 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf),
Table 6-4, lists a 382 mA Wi-Fi TX peak at its stated RF conditions.
RADIO-004's 500 mA remains a design allocation, not a measured universal
maximum. Combining it with the MCU reference screen gives 896.27 mA.
This excludes storage, external IO loading, other rails, display, lighting,
converter losses and charging. It must not be presented as the board's
complete worst-case current or used alone to approve a 2 A converter.

### Reproducible arithmetic

```sh
python3 - <<'PY'
from fractions import Fraction as F
from math import isclose

cases = [(511000,91000,F('.01'),100,3.1134944353990184,3.5086302140788614),
         (56000,10000,F('.001'),25,3.242044111302129,3.3584850935146022)]
for rt, rb, tolerance, tcr, expected_min, expected_max in cases:
    fmin = (1-tolerance)*(1-F(tcr,1_000_000)*100)
    fmax = (1+tolerance)*(1+F(tcr,1_000_000)*100)
    vmin = F('.495')*(1+rt*fmin/(rb*fmax))-F('1e-7')*rt*fmax
    vmax = F('.505')*(1+rt*fmax/(rb*fmin))+F('1e-7')*rt*fmax
    assert isclose(float(vmin), expected_min, abs_tol=1e-12)
    assert isclose(float(vmax), expected_max, abs_tol=1e-12)
    print(f'{rt}/{rb}: {float(vmin):.9f}..{float(vmax):.9f} V')
radio_min = vmin-F('.5')*F('.175')
radio_margin = radio_min-F('3.1320664839416503')
mcu_margin = vmin-F('3.19395125')
assert isclose(float(radio_margin*1000),22.4776273604787,abs_tol=1e-9)
assert isclose(float(mcu_margin*1000),48.0928613021293,abs_tol=1e-9)
assert F('3.3')/66000 == F(50,1_000_000)
assert F('6.27')+390+500 == F('896.27')
print(f'Radio release budget: {float(radio_margin*1000):.6f} mV')
print(f'MCU release budget: {float(mcu_margin*1000):.6f} mV')
print('PWR-002 PASS: screening arithmetic, not regulator or board qualification.')
PY
```
