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
