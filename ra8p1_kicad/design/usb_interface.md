# RA8P1 USB electrical design

## USB-001: R6 reference resistor

Revision 1, 2026-09-05. Applies to U1.J15 and R6 on
[the MCU interface sheet](../ereader/mcu_interfaces.kicad_sch), page 2 of
[the full schematic PDF](../exports/ereader_rev1.pdf). The on-sheet USB-001
annotation links back to this section. Tracking: [#824](https://github.com/bsikar/ra8-firmware/issues/824).

### Requirement and selected component

The [Renesas RA8x2 Quick Design Guide](https://www.renesas.com/en/document/apn/ra8p1-mcu-quick-design-guide),
R01AN7883EU0110 Rev.1.10, Table 2, p.7 specifies a 2.2 kohm +/-1%
resistor from USBHS_RREF to VSS1_USBHS/VSS2_USBHS. This reference sets
the PHY's internal current reference; it is not a USB data termination.
The value is a manufacturer requirement, not a result of dividing the
3.3 V supply by a desired current.

R6 is YAGEO RC0603FR-072K2L: 2.2 kohm +/-1%, 0603, 0.1 W at 70 C,
TCR +/-100 ppm/C, rated component temperature -55 to +155 C.
See the [exact manufacturer specification](https://yageogroup.com/component-documentation/download/specsheet/RC0603FR-072K2L).
The component temperature rating does not establish the product's operating
range. Footprint qualification remains deferred.

U1.J15 and R6.1 share the sheet-local USBHS_RREF net. R6.2 connects to
GND, shared with U1.J16 and U1.J17. Keep the physical resistor close to
the reference pin with a short, quiet ground return during later PCB work.

### Calculation and limits

```text
Rnom = 2200 ohm
t = 1/100 = 0.01
DeltaR = Rnom*t = 2200*0.01 = 22 ohm
Rmin = Rnom*(1-t) = 2200*0.99 = 2178 ohm
Rmax = Rnom*(1+t) = 2200*1.01 = 2222 ohm
```

These are initial resistance-tolerance bounds only. They do not include
temperature drift, aging, or soldering shifts. TCR contributes approximately
`100e-6*abs(T-Tref)` fractional change at its specified maximum magnitude;
do not label the initial +/-1% range as an all-temperature guarantee.

The documents above do not establish the operating voltage across R6.
Consequently this record does not invent a PHY reference current or resistor
dissipation using `3.3 V / 2200 ohm`. Physical operation and temperature
qualification remain unverified.

### Reproduce the Python arithmetic check

Run from the repository root. The check is read-only and also checks the
native KiCad BOM export against the selected reference and part number:

```sh
python3 - <<'PY'
import csv
from fractions import Fraction
from pathlib import Path

r = Fraction(2200)
t = Fraction(1, 100)
actual = (r*t, r*(1-t), r*(1+t))
if actual != (22, 2178, 2222):
    raise ValueError('USB-001 arithmetic mismatch')
with Path('ra8p1_kicad/exports/ereader_rev1_bom.csv').open(newline='') as stream:
    rows = list(csv.DictReader(stream))
row = next(item for item in rows if item['Reference'] == 'R6')
if row['Value'] != '2.2k 1%' or row['Manufacturer_Part_Number'] != 'RC0603FR-072K2L':
    raise ValueError('USB-001 BOM mismatch')
print('USB-001 PASS: +/-22 ohm; 2178..2222 ohm; exact BOM part agrees.')
PY
```

### Procurement snapshot

[DigiKey 311-2.20KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-072K2L/729963),
checked 2026-09-05: Active, 556166 in stock, 17-week standard lead time.
USD unit pricing: qty 1 0.10, qty 10 0.025, qty 100 0.0122.
This is a dated snapshot, not a stock or price guarantee. Native schematic
fields and the exported BOM carry the exact ordering code and source.

## USB-002: Full-speed supply bypass

Revision 1, 2026-09-05. Applies to U1.U17 (VCC_USB), U1.T17 (VSS_USB)
and C40 on the MCU interface sheet. The USB-002 annotation links here.
Renesas R01AN7883EU0110 Table 1, p.6 requires VCC_USB connected to the
3.3 V system supply and bypassed to VSS_USB with a nearby 100 nF capacitor.

Standard +3V3_MCU power symbols connect U1.U17 and the bypass block to the
same global rail, without a second local name or a separate regulated or
filtered rail. C40.1 is on that supply; C40.2 and U1.T17 are on GND.
Place C40 physically close to the supply pin with a short ground return
when the PCB is designed. Separation on this schematic does not prescribe
physical separation. Do not add a PWR_FLAG to conceal the unfinished source
regulator: this bypass is not a power source.

C40 is TDK C1608X7R1H104K080AA, 100 nF +/-10%, 50 V X7R, the same
screened part as the other 3.3 V bypasses. Manufacturer curve provenance,
temperature/aging limitations and nominal DC-bias interpolation are in
[PWR-001](power_decoupling.md#nominal-dc-bias-screening-and-shared-bypass-selection).

```text
C_initial_min = 100*(1-0.10) = 90 nF
C_initial_max = 100*(1+0.10) = 110 nF
C_nominal(3.3 V) = 99.5575 + (3.3-3.15)/0.85*(-0.605)
                 = 99.450735... nF
```

These are respectively initial tolerance bounds and a nominal reference
estimate, not a guaranteed all-corners operating capacitance. Final supply
regulation, USB data routing/connector/protection and the high-speed analog
filter remain separate unfinished requirements. USB-003 below records the
high-speed digital supply capacitors.

Run this read-only check from the repository root:

```sh
python3 - <<'PY'
import csv
from fractions import Fraction as F

with open('ra8p1_kicad/exports/ereader_rev1_bom.csv', newline='') as stream:
    rows = [r for r in csv.DictReader(stream) if 'C40' in r['Reference'].split(',')]
if len(rows) != 1:
    raise ValueError('C40 must occur exactly once')
row = rows[0]
if (row['Value'], row['Manufacturer_Part_Number'], row['DNP']) != (
        '100n', 'C1608X7R1H104K080AA', ''):
    raise ValueError('C40 BOM inputs differ from calculation')
actual = (100*(1-F('0.10')), 100*(1+F('0.10')),
          F('99.5575')+(F('3.3')-F('3.15'))/F('0.85')*F('-0.605'))
if actual != (90, 110, F(676265, 6800)):
    raise ValueError('USB-002 arithmetic mismatch')
print('USB-002 PASS: 90..110 nF initial; 99.450735 nF nominal at 3.3 V.')
PY
```

## USB-003: High-speed digital supply bypass and bulk capacitance

Revision 1, 2026-09-05. Applies to C41/C42 and U1.H15 (VCC_USBHS) on
[the MCU interface sheet](../ereader/mcu_interfaces.kicad_sch). The USB-003
schematic annotation links here. Tracking: [#824](https://github.com/bsikar/ra8-firmware/issues/824).

### Requirement and circuit

[Renesas RA8x2 Quick Design Guide](https://www.renesas.com/en/document/apn/ra8p1-mcu-quick-design-guide),
R01AN7883EU0110 revision 1.10, Table 1 p.6, specifies VCC_USBHS connected
to the 3.3 V system supply, a nearby 10 nF ceramic capacitor, and a 47 uF
electrolytic capacitor, both returning to VSS1_USBHS/VSS2_USBHS.
These are manufacturer-prescribed nominal values, not capacitances derived
from an assumed USB current. The ceramic and electrolytic serve different
frequency ranges; adding their nameplate capacitances does not replace
either requirement.

U1.H15, C41.1 and C42.1 connect to +3V3_MCU. C41.2, C42.2, U1.J17
(VSS1_USBHS) and U1.J16 (VSS2_USBHS) connect to GND. C42 is polarized:
positive terminal to the supply, negative terminal to ground. No PWR_FLAG
is added: capacitors cannot qualify the unfinished regulator as a source.
The separate AVCC_USBHS filtered supply still requires implementation.

### Exact parts and initial tolerance

C41: [TDK C1608X7R1H103K080AA](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1608X7R1H103K080AA),
10 nF +/-10%, 50 VDC, X7R (+/-15%), -55 to +125 C, commercial 0603.
TDK lists Production. The exact part's voltage-bias curve is screened below;
the curve for the 100 nF sibling must not be substituted.

C42: [Panasonic EEE-FP0J470AR](https://industrial.panasonic.com/ww/products/pt/aluminum-cap-smd/models/EEEFP0J470AR),
manufacturer's compact spelling EEEFP0J470AR. This is a polarized 47 uF
+/-20%, 6.3 V aluminum electrolytic. The [FP-A catalog](https://industrial.panasonic.com/cdbs/www-data/pdf/RDE0000/ABA0000C1184.pdf),
pp.1 and 3, specifies tolerance at 120 Hz/+20 C, ESR <=0.36 ohm at
100 kHz/+20 C, rated ripple 240 mArms at 100 kHz/+105 C, and 2000-hour
endurance at +105 C. Those test conditions are not an e-reader lifetime
prediction. Its 5 mm diameter and 5.8 mm nominal body length are a later
enclosure constraint; footprint qualification remains out of scope.

```text
C41_min = 10*(1-0.10) = 9 nF
C41_max = 10*(1+0.10) = 11 nF
C42_min = 47*(1-0.20) = 37.6 uF
C42_max = 47*(1+0.20) = 56.4 uF

At nominal rail 3.3 V:
C42 voltage utilization = 3.3/6.3*100 = 52.380952 percent
At a 3.6 V screening ceiling (not a measured regulator output):
C42 voltage utilization = 3.6/6.3*100 = 57.142857 percent
C42 voltage headroom = 6.3-3.6 = 2.7 V
C41 voltage utilization = 3.6/50*100 = 7.2 percent
```

The tolerance intervals are initial component limits, not guaranteed
operating capacitance after temperature, reflow, aging or endurance.
Supply transients must remain within both MCU and capacitor limits, and
C42 must never see reverse voltage. Extra capacitor voltage rating does
not increase the MCU's permissible supply voltage.

### C41 nominal DC-bias screening

The exact TDK product page's DC Bias Characteristic CSV was downloaded in
the browser on 2026-09-05. The [preserved source CSV](../resources/datasheets/TDK_C1608X7R1H103K080AA_dc_bias_2026-09-05.csv)
retains all data rows; only BOM, line endings and trailing blank row are
normalized. TDK labels this reference data, not guaranteed characteristics.

```text
Manufacturer samples: C(3.15 V) = 10.0899 nF; C(4 V) = 10.0570 nF
Linear interpolation, C(V) = Ca + (V-Va)/(Vb-Va)*(Cb-Ca):
C(3.3 V) = 10.0899 + (3.3-3.15)/(4-3.15)*(10.0570-10.0899)
         = 10.084094117647... nF
C(3.6 V) = 10.0899 + (3.6-3.15)/(4-3.15)*(10.0570-10.0899)
         = 10.072482352941... nF
```

This supports the initial low-bias selection, not an all-corners minimum.
Do not claim that the slight nominal increase compensates initial tolerance
or that bias characteristics of other capacitances are interchangeable.

### Remaining electrical qualification

Place C41 close to H15 with a short return to the USB-HS ground pins;
keep C42's supply/return loop compact. Drawing separation is not a physical
placement instruction. Final regulator ramp, rail tolerance, load transients,
capacitor ripple heating and endurance must be checked against the finished
power tree and operating temperature. Ripple current is not the MCU DC load
current. This record does not assume a current step, derive fictitious droop,
or certify USB signal integrity from capacitance values.

### Procurement snapshot

Checked 2026-09-05, USD, before shipping/tax/tariff:

| Ref | DigiKey cut-tape part | Status / stock | Standard lead | Unit price at 1 / 10 / 100 |
| --- | --- | --- | --- | --- |
| C41 | [445-1311-1-ND](https://www.digikey.com/en/products/detail/tdk/C1608X7R1H103K080AA/567691) | Active / 512826 | 24 weeks | 0.10 / 0.055 / 0.0323 |
| C42 | [PCE4511CT-ND](https://www.digikey.com/en/products/detail/panasonic-industry/EEE-FP0J470AR/1701010) | Active / 11266 | 37 weeks | 0.65 / 0.408 / 0.2699 |

Stock supports prototype quantities; long replenishment times remain a
procurement risk. This snapshot is not a future availability guarantee.

### Reproducible arithmetic and BOM check

Run from the repository root after exporting the native BOM:

```sh
python3 - <<'PY'
import csv
from fractions import Fraction as F
from pathlib import Path

with Path('ra8p1_kicad/exports/ereader_rev1_bom.csv').open(newline='') as stream:
    bom = list(csv.DictReader(stream))
for ref, value, mpn, source in (
        ('C41', '10n', 'C1608X7R1H103K080AA', '445-1311-1-ND'),
        ('C42', '47u 6.3V', 'EEE-FP0J470AR', 'PCE4511CT-ND')):
    matches = [r for r in bom if ref in r['Reference'].split(',')]
    if len(matches) != 1:
        raise ValueError(ref + ' must occur exactly once')
    r = matches[0]
    if (r['Value'], r['Manufacturer_Part_Number'], r['DigiKey_Part_Number'], r['DNP']) != (
            value, mpn, source, ''):
        raise ValueError(ref + ' BOM differs from calculation inputs')

path = Path('ra8p1_kicad/resources/datasheets/TDK_C1608X7R1H103K080AA_dc_bias_2026-09-05.csv')
with path.open(newline='') as stream:
    data = [r for r in csv.reader(stream) if r]
if data[0] != ['TDK Corporation'] or data[3] != ['C1608X7R1H103K080AA']:
    raise ValueError('Wrong manufacturer curve identity')
if data[4] != ['DC/V', 'Capacitance(Nom.)/F']:
    raise ValueError('Wrong curve units')
samples = {F(v): F(c)*10**9 for v, c in data[5:]}
if (samples[F('3.15')], samples[F(4)]) != (F('10.0899'), F('10.057')):
    raise ValueError('Curve input samples changed')
for voltage, expected in (('3.3', F(214287, 21250)), ('3.6', F(856161, 85000))):
    result = samples[F('3.15')] + (F(voltage)-F('3.15'))/F('.85')*(samples[F(4)]-samples[F('3.15')])
    if result != expected:
        raise ValueError('DC-bias interpolation mismatch')
actual = (10*(1-F('.1')), 10*(1+F('.1')), 47*(1-F('.2')), 47*(1+F('.2')),
          F('3.3')/F('6.3')*100, F('3.6')/F('6.3')*100,
          F('6.3')-F('3.6'), F('3.6')/50*100)
if actual != (9, 11, F('37.6'), F('56.4'), F(1100, 21), F(400, 7), F('2.7'), F('7.2')):
    raise ValueError('USB-003 tolerance or voltage arithmetic mismatch')
print('USB-003 PASS: C41/C42 BOM, tolerance, voltage screening and nominal bias arithmetic.')
print('Hardware rail, ripple and lifetime qualification remain open.')
PY
```
