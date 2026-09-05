# Power decoupling engineering records

## PWR-001: C39 MIPI analog-supply bypass

Revision 1, 2026-09-05. Applies to C39 and U1 unit M on
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
Review the exact-part characteristic data and final rail specification before
promoting this candidate to qualified. Layout must provide a short local
bypass loop between the named supply and ground pins; that is a later PCB task.

### Reproducible Python arithmetic and BOM check

Run from the repository root. This checks only the stated arithmetic and
exact exported BOM inputs; it does not modify the design or simulate a PDN.

```sh
python3 - <<'PY'
import csv
from fractions import Fraction as F

with open('ra8p1_kicad/exports/ereader_rev1_bom.csv', newline='') as stream:
    rows = [r for r in csv.DictReader(stream) if 'C39' in r['Reference'].split(',')]
if len(rows) != 1:
    raise ValueError('C39 must occur exactly once in BOM')
row = rows[0]
if (row['Value'], row['Manufacturer_Part_Number'], row['DNP']) != (
        '100n', 'C1608X7R1H104K080AA', ''):
    raise ValueError('C39 calculation inputs differ from BOM')
checks = ((100*(1-F(1, 10)), 90), (100*(1+F(1, 10)), 110),
          (F(33, 10)/50*100, F(33, 5)), (F(36, 10)/50*100, F(36, 5)))
for actual, expected in checks:
    if actual != expected:
        raise ValueError((actual, expected))
print('PWR-001 PASS: 90..110 nF initial; voltage utilization 6.6% / 7.2%.')
print('Operating capacitance and final rail qualification remain open.')
PY
```
