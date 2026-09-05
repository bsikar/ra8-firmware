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
filter and supply capacitors remain separate unfinished requirements.

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
