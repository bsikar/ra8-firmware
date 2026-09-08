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

Revision 2, 2026-09-08. Linked from the PWR-002 annotation on the
[radio sheet](../ereader/radio_esp32.kicad_sch). Tracking:
[power #825](https://github.com/bsikar/ra8-firmware/issues/825),
[architecture #823](https://github.com/bsikar/ra8-firmware/issues/823), and
[radio #826](https://github.com/bsikar/ra8-firmware/issues/826).
This voltage screen is implemented by U13/R41/R42 on the
[main supply sheet](../ereader/main_3v3_supply.kicad_sch); PWR-003 records
the complete local circuit and its remaining qualification boundaries.
It is not a qualified system power supply.

### Source limits and operating-mode boundary

[TI TPS63802 SLVSEU9D](https://www.ti.com/lit/ds/symlink/tps63802.pdf),
section 8.5, specifies 500 mV nominal feedback, +/-1% accuracy in PWM mode,
and 100 nA maximum feedback bias at 500 mV. Section 10.2.2.5 limits the
bottom feedback resistor to 100 kohm. The 511k/91k reference divider is
not an exact 3.300 V setting. PG's 95% rising/90% falling thresholds are
typical; PG does not replace a guaranteed reset threshold.

The candidate's +/-1% PWM specification must not be applied to low-load
PFM behavior. U13 MODE is now tied directly to VIN, selecting forced PWM
whenever enabled. Startup, transitions and the sleep-mode rail envelope
still require verification. Any later firmware-controlled or power-saving
MODE change reopens this budget. The earlier assumed 3.3 V +/-2% screen
is not a completed regulator specification.

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
U6 maximum rising screen, RADIO-014 fitted divider = 3.139621574 V
Remaining radio release budget = 14.922537 mV
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
radio_margin = radio_min-F('3.1396215743674185')
mcu_margin = vmin-F('3.19395125')
assert isclose(float(radio_margin*1000),14.9225369347108,abs_tol=1e-9)
assert isclose(float(mcu_margin*1000),48.0928613021293,abs_tol=1e-9)
assert F('3.3')/66000 == F(50,1_000_000)
assert F('6.27')+390+500 == F('896.27')
print(f'Radio release budget: {float(radio_margin*1000):.6f} mV')
print(f'MCU release budget: {float(mcu_margin*1000):.6f} mV')
print('PWR-002 PASS: screening arithmetic, not regulator or board qualification.')
PY
```

## PWR-003: TPS63802 main digital converter

Revision 3, 2026-09-08. Tracking: [power #825](https://github.com/bsikar/ra8-firmware/issues/825).
This is the implementation basis for the main converter in the
[e-reader project](../ereader/ereader_rev1.kicad_sch). The local library symbol
is `Power_Devices:TPS63802DLA`, with TPS63802DLAR as its exact default part.
The circuit and linked PWR-002/PWR-003 calculation annotations are implemented
on [Main 3V3 digital supply](../ereader/main_3v3_supply.kicad_sch), page 9.
U13 EN connects through root-sheet pins to the existing U9/U11/R34/U12
MAIN_PWR_EN network. This remains a conditional schematic selection, not a
completed power tree, measured current envelope or fabrication release.
The preceding PWR-002 voltage arithmetic remains authoritative. This
section supersedes its tentative MODE control with **MODE tied to VIN**:
forced PWM whenever the converter operates, including low-load operation.
Any later power-saving mode change reopens the voltage and reset budgets.
Use with [SYS-007/008](system_power_design.md),
[memory/camera allocation](camera_storage_interfaces.md) and
[audio power](audio_subsystem.md#aud-005-audio-rails-thermal-load-and-power-path-impact).
This revision reconciles the load, startup and thermal screens with the
selected CMS-011 NOR's 250 mA allocation. It does not change the native
TPS63802 circuit, its component values or its qualification status.

### Connections and exact candidate parts

[TI SLVSEU9D, Table 7-1 and sections 8.3, 8.5, 9.4 and 10.2](https://www.ti.com/lit/ds/symlink/tps63802.pdf)
provide the converter requirements and pin contract:

| TPS63802DLAR pin | Connection |
| --- | --- |
| 1 EN | MAIN_PWR_EN from the SYS-007 open-drain control network |
| 2 MODE, 10 VIN | Raw SYS_AON; never AON_HOLD or a firmware-driven MODE net |
| 3 AGND, 8 GND | Common electrical GND; keep noisy power and feedback return paths controlled in layout |
| 4 FB | Junction of 56k from VOUT and 10k to GND |
| 5 PG | Open-drain status; not a substitute for the existing reset supervisor; leave explicitly unused unless its receiver/pullup are designed |
| 6 VOUT | +3V3_MCU with two local 22 uF capacitors to GND |
| 7 L2, 9 L1 | Opposite terminals of the dedicated 0.47 uH inductor; neither terminal is a ground or output-rail connection |
| VIN bypass | One local 10 uF capacitor from VIN to GND |

Native references: U13 converter; L2 inductor; C73 input bypass; C74/C75
output bypass; R41 upper and R42 lower feedback resistors. U13 PG carries
an explicit no-connect flag. +3V3_MCU is driven by U13's power-output pin;
no artificial power flag was added to that rail. SYS_AON's upstream source
remains a separate unfinished circuit. The instances retain exact BOM
fields and sourcing snapshots; footprint assignments remain deferred.

| Function / quantity | Exact part | Primary electrical source |
| --- | --- | --- |
| Converter / 1 | TPS63802DLAR | [TI datasheet](https://www.ti.com/lit/ds/symlink/tps63802.pdf) |
| Input capacitor / 1 | TDK C3216X7R1V106K160AC, 10 uF, 35 V, X7R, +/-10% | [TDK product record](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C3216X7R1V106K160AC), [characterization sheet](https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/c3216x7r1v106k160ac.pdf) |
| Output capacitors / 2 | Murata GRM32ER71C226KEA8L, 22 uF, 16 V, X7R, +/-10% | [Murata reference specification](https://search.murata.co.jp/Ceramy/image/img/A01X/G101/ENG/GRM32ER71C226KEA8-01A.pdf) |
| Inductor / 1 | Murata DFE322520F-R47M=P2, 0.47 uH, +/-20% | [Murata J(E)TE243A-0040D-01](https://pim.murata.com/asset/pim4/inductor/J%28E%29TE243A-0040_PDF_INDUCTOR) |
| Feedback top / 1 | Susumu RG1608P-563-B-T5, 56k, +/-0.1%, 25 ppm/C | [Susumu RG series](https://www.susumu.co.jp/common/pdf/n_catalog_partition01_en.pdf) |
| Feedback bottom / 1 | Yageo RT0603BRD0710KL, 10k, +/-0.1%, 25 ppm/C | [Yageo exact specification](https://www.yageogroup.com/component-documentation/download/specsheet/RT0603BRD0710KL) |

These exact resistors realize the preferred PWR-002 tolerance/TCR screen;
ordinary 1% substitutes do not. Both have 0.1 W standard rated power;
Susumu's element-voltage ceiling is 100 V, Yageo's is 75 V. At a deliberately
conservative 3.6 V across either resistor, including PWR-002's minimum
resistance factor 0.9965025, power is at most 0.232241 mW in 56k and
1.300549 mW in 10k. These are stress screens, not actual divider losses;
temperature derating and long-term drift still apply. Nominal divider
current is 50 uA. No footprint or purchasing approval is implied here.

Required effective capacitances are CIN >=4 uF and COUT >=7 uF at this
output voltage. Use the following **qualification acceptance model**, not
an assertion that the vendor guarantees 60% capacitance retention:

```text
CIN_model = 10u * 0.90 * 0.85 * 0.60 = 4.590 uF
COUT_model = 2*22u * 0.90 * 0.85 * 0.60 = 20.196 uF
```

Here 0.90 is initial tolerance, 0.85 is the X7R temperature factor, and
0.60 is an allocated residual factor for DC bias, aging, excitation,
assembly and other effects not already counted. Qualify the combined
installed capacitance at VIN through 4.6 V and the complete output-voltage
envelope; nominal curves alone cannot establish that minimum. Stability,
ripple-current heating and the aggregate output capacitance remain checks.

The inductor specification lists 16 mOhm maximum DCR, 8.5 A at 30%
inductance reduction and 6 A at 40 C temperature rise on its test board.
Those definitions do not establish hot switching-current capability or
the required **0.37..0.57 uH installed effective inductance**. Initial
0.47 uH +/-20% alone is 0.376..0.564 uH; reflow can change L by +/-10%,
and the specified temperature coefficient is up to 1000 ppm/C. DC bias
adds another dependency. This candidate therefore needs Murata/TI-supported
corner validation or replacement before release. Do not use typical
switching frequency, nominal saturation current or output current as a
guaranteed peak-inductor-current proof.

Forced PWM permits reverse current while enabled. No external supply,
service adapter or separately powered peripheral may drive +3V3_MCU.
Qualified signal isolation remains necessary. EN shutdown uses SYS-007's
separate KILL and discharge paths; fixed MODE does not replace them.

### Main-domain load allocation and mandatory exclusions

The following are design acceptance ceilings, not guaranteed component
maxima or installed hardware current limiters. Verify the selected devices,
clock settings, enabled peripherals, patterns and temperature against them;
if exceeded, revise the supply or domain split before release.

| +3V3_MCU consumer | Continuous allocation | Basis / qualification boundary |
| --- | ---: | --- |
| RA8P1, all connected 3.3 V supply pins and driven IO | 750 mA | Includes core-converter input, PHY/analog, BGO and IO; see accounting below |
| ESP32 radio branch | 500 mA | RADIO-004 acceptance allocation, not an autonomous 500 mA clamp |
| SDRAM including output switching and pulls | 250 mA | [CMS-010](camera_storage_interfaces.md#cms-010-is42s32160f-7tli-pin-and-electrical-contract): selected ISSI -7 IDD4 maximum is 210 mA with outputs open; bus charging and 4 mA pull allocation must fit |
| NOR including output loading and pulls | 250 mA | [CMS-011](camera_storage_interfaces.md#cms-011-128-mib-octal-nor-electrical-contract): S28HL01GTFPBHI030, 128 MiB Octal; qualification allocation, not a guaranteed maximum |
| Small sensors, control logic, pulls and feedback | 50 mA | Component selection and all asserted-low pull currents must fit |
| **Total** | **1800 mA** | **200 mA / 10% below the nominal 2 A rating; not guaranteed delivery or transient margin** |

[Renesas Rev.1.30, Tables 2.8, 2.32 and 2.39](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet)
separate the 396.27 mA ICC+ICC_DCDC reference from unloaded-IO PHY/analog
currents. USBHS adds 55.3 mA maximum; CSI AVCC_MIPI adds 15.3 mA at 95 C.
The BGO table's 105 C high-speed OTP-write screen adds 80 mA at ICC plus
0.5 mA internal IDD; the latter is not directly a 3.3 V input current.
With another 20 mA analog allocation, 183.13 mA of the 750 mA allowance
remains for external IO, core-converter efficiency differences and that
BGO core increment. This remainder is not a measured margin. Do not add
the 1000 mA internal IDD ceiling again as a 3.3 V source load.

The earlier 100 mA Winbond Quad candidate allocation is superseded by
**250 mA for the selected Infineon S28HL01GTFPBHI030 Octal NOR**.
[CMS-011](camera_storage_interfaces.md#cms-011-128-mib-octal-nor-electrical-contract)
owns its exact pin/reset/sourcing contract and current evidence. Its
[released manufacturer datasheet Rev. AB, Table 87, pp.132-134](https://www.mouser.com/datasheet/3/70/1/8HS01GT_S28HL512T_S28HL01GT_512MB_1GB_SEMPER_TM_FLASH_OCTAL_INTERFACE_1_8V_3-DataSheet-v68_00-EN.pdf)
excludes output switching from read-current figures. The 173 mA DDR row
is labeled 200 MHz for both voltage families although the selected HL
part is limited to 166 MHz. It therefore does not provide a clean
guaranteed bound for the proposed 125 MHz DDR/DS operating point.
CMS-011's illustrative nine-output, 15 pF, 125 MHz charge screen adds
56.674436 mA, giving 229.674436 mA with that ambiguous row. This is
arithmetic for qualification planning, not a simulation or manufacturer
guarantee. The 1 mA NOR pull/leakage allowance is inside the 250 mA;
do not count it again in the separate logic row. SDRAM's 4 mA pull
allowance likewise remains inside its 250 mA. Neither lowering the clock
nor multiplying typical current by a factor proves the complete envelope.
The selected Octal capacity/performance and required audio/radio features
are not silently reduced to preserve the previous power total.

Do not connect microSD, camera-module, ESS DAC/clock, headphone/speaker,
e-paper controller/HV, touch or warm/cool frontlight power to this converter.
They remain required features on separately budgeted, switched SYS_AON-
derived domains. In particular, use a separate 3.3 V buck-boost branch for
microSD, initially targeting 500 mA capability with controlled inrush and
a qualified limiter. A raw-SYS LDO cannot maintain 3.3 V near source cutoff.
[Kingston's card specification, Table 6-5](https://www.mouser.com/catalog/specsheets/Kingston_4900279B_SDCIT2_8_to_64GB_1.pdf)
documents 300 mA peaks over 10 us at 25 C even in HS/DS; protocol current
selection alone is not a universal instantaneous card-current guarantee.
The supported-card/ramp envelope remains a separate qualification task.

Keep MCU AVCC_MIPI within its 750 mA allowance. Its separate 1.8 V PHY
supply requires its own sequenced design. ESS DVDD is internally supplied;
its [datasheet, pp.10 and 49-50](https://www.mouser.com/datasheet/3/3763/1/ES9039Q2M_Datasheet_v0.2.2.pdf)
does not provide a maximum DAC-power guarantee from the typical 78 mW
mode example. Unknown camera, panel and touch currents are not zero.
Each separate domain needs hard-off/discharge and powered-off IO checks;
moving its load off this converter does not remove it from SYS/battery
accounting or approve full-power simultaneous operation.

### Source, transient and thermal screens

SYS-007's lowest calculated running trip is 3.263705831 V at its sensed
raw node. Allocate VIN at the converter >=3.20 V in normal operation,
leaving 63.705831 mV for path loss at that corner. This is a routing/load
acceptance limit, not protection against a fast source collapse. Retain
SYS-007's complete source-loss shutdown and hold-up qualification.

At PWR-002's PWM static high corner, 3.358485094 V:

```text
Pout = 3.3584850935 * 1.80 = 6.045273 W
Pin = Pout / eta; Iin = Pin / VIN; Ploss = Pin - Pout
```

| Assumed efficiency | Input at raw trip | Input at VIN=3.20 V | Total conversion loss | Loss * TI reference 81 C/W |
| --- | ---: | ---: | ---: | ---: |
| 85% | 2.179144 A | 2.222527 A | 1.066813 W | 86.411846 C |
| 90% | 2.058081 A | 2.099053 A | 0.671697 W | 54.407459 C |
| 93% | 1.991691 A | 2.031342 A | 0.455021 W | 36.856665 C |

Efficiency values are assumptions, not lower bounds. Charging all converter
loss to the IC is a conservative heat-allocation screen; actual IC and
inductor losses differ. TI's reference-board thermal resistance is not
the finished enclosure's thermal model. Demonstrate operation below the
125 C recommended converter junction limit with design margin and below
every other component's limit. Do not design normal operation around
thermal shutdown. The 2 A / 2.3 V full-load datum is at 25 C and 3.3 V;
typical output-capability curves do not establish our all-corners rating.

PWR-002 leaves only 14.922537 mV radio release and 48.092861 mV MCU
release allocation. Ripple, path loss, startup and load steps must fit the
applicable thresholds; 200 mA of nameplate current headroom is not 200 mV of voltage
headroom. Test at the converter and at the load/supervisor pins.

The increased allocation reopens approval of this converter and the
upstream source budget. Retaining TPS63802 requires demonstrated output
delivery at the minimum VIN, actual inductor/capacitor corners, startup
and transient loads, and enclosure thermal conditions. If those cannot
be met with margin, a higher-capability converter or a reviewed domain
split is an architecture decision, not a drop-in substitution or a
reason to lower required product performance without review. This
document does not select a replacement or claim the native circuit was
changed. The battery/charger/source must also account for this higher
input current plus every separately powered mandatory domain.

### Cold start and wake are separate current cases

[Renesas Table 2.38, p.79](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet)
gives VCC_DCDC cold-start inrush 1.330 A and deep-standby-return references
1.270/1.170/1.160 A. Its reference-only note applies despite the table's
maximum columns. These are **not whole-MCU/whole-rail current guarantees**.

```text
Cold-start subtotal = 1.330 + 0.250 + 0.250 + 0.050 = 1.880 A
Same subtotal with radio on = 1.880 + 0.500 = 2.380 A (prohibited)
Largest wake-reference subtotal, radio off = 1.270 + 0.550 = 1.820 A
```

The first subtotal leaves only 120 mA before the nominal 2 A rating,
and excludes non-DCDC MCU start current and capacitor charging not already
represented in the reference measurement. It is not a safe-start proof.
Do not add the complete 750 mA steady MCU allocation to the DCDC inrush:
that would double-count overlapping consumption. Instead characterize
separate DCDC, remaining MCU supply and additional charging contributions.
For additional uncounted capacitance, use Icharge = Cadditional*dV/dt,
with the measured/qualified ramp rather than TI's typical soft-start time.

Radio must be hardware-default off at cold start and commanded off before
deep-standby entry so it remains off through the wake inrush. Delay its
enable, and the separate peripheral supply ramps, until MCU startup and
the main rail settle. Qualification must cover ROM/USB recovery, all wake
modes, prebiased outputs and repeated interruptions. Total directly
connected main-rail capacitance must still fit SYS-007's <=1 mF discharge
allocation; capacitance is not free inrush or shutdown margin.

### Python verification and schematic note

This block checks arithmetic and declared boundary conditions only. It
does not edit KiCad, simulate the converter or qualify components.

```python
from math import isclose
from pathlib import Path
import re

main_alloc = dict(mcu=.750, radio=.500, sdram=.250, nor=.250, logic=.050)
# Bind this calculation to the displayed power table and selected NOR contract.
document = Path('ra8p1_kicad/design/power_decoupling.md').read_text()
section = document.split('## PWR-003: TPS63802 main digital converter')[1]
allocation_rows = re.findall(r'^\| ([^|]+) \| (\d+) mA \|', section, re.M)
assert len(allocation_rows) == 5
assert {name.split()[0]: int(ma)/1000 for name, ma in allocation_rows} == {
    'RA8P1,': main_alloc['mcu'], 'ESP32': main_alloc['radio'],
    'SDRAM': main_alloc['sdram'], 'NOR': main_alloc['nor'],
    'Small': main_alloc['logic'],
}
memory_document = Path('ra8p1_kicad/design/camera_storage_interfaces.md').read_text()
nor_contract = memory_document.split('## CMS-011: 128 MiB Octal NOR electrical contract')[1]
nor_ma = int(re.search(r'Reserve \*\*(\d+) mA for this NOR domain\*\*', nor_contract).group(1))
assert main_alloc['nor'] == nor_ma/1000
imain = sum(main_alloc.values())
vout_hi = 3.3584850935146022  # unchanged PWR-002 result
vtrip_lo = 3.263705830523478  # SYS-007 raw-node result
vin_alloc = 3.20
pout = vout_hi * imain
mcu_named = .39627 + .0553 + .0153 + .080 + .020
other_start = main_alloc['sdram'] + main_alloc['nor'] + main_alloc['logic']
cold_ref = 1.330 + other_start
precision_lo = (1-.001)*(1-25e-6*100)
checks = {
    'continuous main allocation A': (imain, 1.80),
    'nameplate headroom A': (2-imain, .200),
    'nameplate headroom percent': ((2-imain)/2*100, 10.0),
    'MCU unqualified remainder A': (.750-mcu_named, .18313),
    'output power high screen W': (pout, 6.045273168326284),
    'raw-to-VIN path allocation V': (vtrip_lo-vin_alloc, .06370583052347767),
    'cold reference subtotal A': (cold_ref, 1.880),
    'prohibited cold plus radio subtotal A': (cold_ref+main_alloc['radio'], 2.380),
    'wake reference subtotal A': (1.270+other_start, 1.820),
    'cold subtotal remaining A': (2-cold_ref, .120),
    'CIN conditional effective uF': (10*.9*.85*.6, 4.590),
    'COUT conditional effective uF': (2*22*.9*.85*.6, 20.196),
    'inductor initial low uH': (.47*.8, .376),
    'inductor initial high uH': (.47*1.2, .564),
    '56k full-3.6V power screen W': (3.6**2/(56000*precision_lo), .00023224083374459315),
    '10k full-3.6V power screen W': (3.6**2/(10000*precision_lo), .0013005486689697217),
}
for label, (actual, expected) in checks.items():
    assert isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-12), label
    print(label, f'{actual:.12g}')
thermal_cases = (
    (.85, 2.1791443376632786, 2.2225269001199575, 1.0668129120575802, 86.411845876664),
    (.90, 2.0580807633486518, 2.0990531834466264, .6716970187029201, 54.40745851493653),
    (.93, 1.9916910613051466, 2.0313417904322186, .4550205610568163, 36.85666544560212),
)
for eta, iraw, iatvin, loss, trise in thermal_cases:
    pin = pout/eta
    actual = (pin/vtrip_lo, pin/vin_alloc, pin-pout, 81*(pin-pout))
    assert all(isclose(a, b, rel_tol=1e-10) for a, b in zip(actual, (iraw, iatvin, loss, trise)))
    print('assumed eta / Iraw / IVIN / loss / reference rise', eta, *actual)
assert imain < 2 < cold_ref+main_alloc['radio']
assert 10*.9*.85*.6 >= 4 and 2*22*.9*.85*.6 >= 7
assert .37 <= .47*.8 < .47*1.2 <= .57  # INITIAL tolerance only
assert checks['56k full-3.6V power screen W'][0] < .1
assert checks['10k full-3.6V power screen W'][0] < .1
print('PWR-003 arithmetic PASS; L/C, current, startup and thermal qualification remain open.')
```

Two calculation annotations beside the native circuit provide the bidirectional
document link:

- `PWR-002 / PWR-003: SETPOINT AND LOCAL COMPONENTS` shows the 56k/10k
  nominal setpoint and divider current, complete tolerance/TCR/feedback-bias
  voltage-corner formulas, conditional effective-capacitance equations,
  and initial versus installed-inductance limits.
- `PWR-003: LOAD, SOURCE AND STARTUP ACCEPTANCE LIMITS` shows this
  revision's 1.80 A total, 2.222527 A input at 3.20 V / assumed 85%,
  1.066813 W loss / 86.411846 C reference rise, and 1.880/1.820 A
  radio-off cold/wake subtotals. The annotation was updated in native
  KiCad together with U13's selection-basis field. It retains the
  separate-domain exclusions, input path-loss allocation,
  additional-capacitance charging equation, discharge-capacitance ceiling
  and release-margin qualification gates.

Both point back to this section and its executable Python. Changes to values,
allocation assumptions or conclusions require updating the corresponding
native annotation and rerunning PWR-002/PWR-003; these screens are not a
simulation or a measured qualification result.
