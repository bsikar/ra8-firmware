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
