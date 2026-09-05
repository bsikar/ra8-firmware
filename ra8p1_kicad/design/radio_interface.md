# ESP32-C6 radio electrical interface

Design record for issue #826 and schematic `radio_esp32.kicad_sch`.
The retained module candidate is ESP32-C6-WROOM-1-N8. This record is an
interface design basis, not a completed circuit or demonstrated RA8P1 port.

## RADIO-001: host transport

ESP-Hosted supports a full-duplex SPI coprocessor connection on ESP32-C6.
It requires SCLK, COPI, CIPO, CS, reset, handshake and data-ready. Both
handshake and data-ready need interrupt-capable host inputs. A four-wire
SPI connection alone is insufficient. A non-Espressif host needs a software
port; protocol availability does not prove an existing RA8P1 implementation.

The documented default C6 coprocessor mapping is:

| Signal | C6 GPIO | Module contact | Direction relative to RA8P1 |
| --- | --- | --- | --- |
| RADIO_SCLK | 6 | 6 | Output |
| RADIO_COPI | 7 | 7 | Output |
| RADIO_CIPO | 2 | 27 | Input |
| RADIO_CS_N | 10 | 11 | Output |
| RADIO_HANDSHAKE | 3 | 26 | Input, interrupt |
| RADIO_DATA_READY | 4 | 4 | Input, interrupt |
| RADIO_EN | EN | 3 | Reset control; electrical drive circuit pending |

This is a candidate mapping, not the final RA8P1 pin allocation. GPIO4 is
also a strap and must not be loaded by a host pull during strap sampling.
GPIO6/7 also have JTAG functions. Keep recovery programming separate from
these reused signals. SDIO remains an alternative until host peripheral
allocation and throughput requirements are reviewed. Do not label SPI
frequency as achieved throughput: framing, handshakes and firmware reduce it.

Source: [Espressif Hosted full-duplex SPI guide](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/spi_full_duplex.md),
sections 3.1 and 5, read 2026-09-05. Pin defaults are configurable and must
be frozen with the coprocessor firmware configuration before release.

## RADIO-002: module pin and boot constraints

Authority: [Espressif module datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf),
ESP32-C6-WROOM-1 / WROOM-1U v1.4, Tables 3-1, 4-2, 4-3, 4-8 and 6-2.

- Supply contact 2: 3.0..3.6 V; external supply capability at least 0.5 A.
  This is a supply-capability requirement, not a constant consumption claim.
- Ground contacts: 1, 28 and exposed pad 29. Contact 22 is NC.
- EN must not float. Supply stabilization before EN rises and reset-low
  duration each require at least 50 us. Strap hold time after EN rises is
  at least 3 ms.
- Normal flash boot requires GPIO9 high. Joint download requires GPIO8
  high and GPIO9 low. Provide deterministic strap levels and recovery access.
- UART0 RX/TX are contacts 24/25. GPIO12/13 are USB D-/D+ on contacts
  13/14. Select the recovery interface explicitly; neither is automatically
  safe to connect to an independently powered programming adapter.

The imported local symbol numbered exposed-pad subdivisions `29_1`,
`29_2`, etc. These were not distinct manufacturer electrical pins. The
project-local symbol was corrected through KiCad to one EPAD pin 29,
with the eight redundant entries removed. All contacts 1..29 are present
once and the GPIO/control mapping was checked against Table 3-1. The
power-unit outline was reduced to fit its four remaining pins. U3A/U3B
are placed on the radio sheet; grounds and manufacturer-NC connections are
captured. Contact 2 and C45's upper terminal connect to +3V3_RADIO;
C45's lower terminal connects to GND, verified in the saved netlist.
C45 reuses TDK C1608X7R1H104K080AA (100 nF, 50 V, X7R, +/-10%) and
its existing sourced BOM metadata. This local bypass does not replace bulk
capacitance, supply transient analysis, or the required source circuitry.
C46 adds a 10 uF nominal bulk capacitor across the same rail and ground;
its bias calculation is RADIO-003 below. The host interface and remaining
support circuit are still open.
Physical pad segmentation and footprint mapping require the separate
deferred PCB review; the existing imported footprint is not qualified by
this electrical correction.

## Power-domain integration

The module rail is named +3V3_RADIO; its source and enable/control policy
are not yet implemented. No PWR_FLAG declares the unfinished rail driven. Every
host-driven signal, pull-up, recovery signal and interrupt return must be
reviewed for both host-off/radio-on and host-on/radio-off states. Firmware
high-impedance configuration alone is not a guaranteed power-off isolation
mechanism. Select isolation with specified partial-power-down behavior if
the rails can be independently switched. Account for its propagation delay,
leakage, default output-enable state and loading in the final timing budget.

The module's antenna and 40 MHz oscillator are integrated. Do not add a
second external main oscillator or discrete RF matching network to its
digital carrier schematic. Mechanical antenna clearance remains a later
integration constraint, not a justification for arbitrary footprint changes.

## RADIO-003: bulk capacitance

Revision 1, 2026-09-05. Applies to C46 on the
[radio schematic](../ereader/radio_esp32.kicad_sch), whose RADIO-003 note
links to this section. The exact part is TDK C3216X7R1V106K160AC,
10 uF nominal, +/-10%, 35 V, X7R. This reuses the sourced part fitted at
C43, not the USB circuit's selection rationale. C45 supplies the parallel
100 nF local bypass. Both capacitors connect between +3V3_RADIO and GND.

The [TDK exact-part record](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C3216X7R1V106K160AC)
provides the nominal DC-bias curve retained in the
[source CSV](../resources/datasheets/TDK_C3216X7R1V106K160AC_dc_bias_2026-09-05.csv).
Its adjacent points are 9.69875 uF at 3.15 V and 9.38950 uF at 4 V.
Linear interpolation gives:

```text
C(V) = Ca + (V - Va)/(Vb - Va)*(Cb - Ca)
C(3.3 V) = 9.69875 + (3.3 - 3.15)/0.85*(9.38950 - 9.69875)
         = 9.644176470588... uF
C(3.6 V) = 9.69875 + (3.6 - 3.15)/0.85*(9.38950 - 9.69875)
         = 9.535029411764... uF
```

These values are nominal interpolation, NOT a guaranteed minimum. Initial
tolerance, temperature, aging and excitation are not included. The 35 V
rating alone does not establish effective capacitance. The module's supply
capability requirement is at least 0.5 A; this is not an assumed load-step
amplitude or proof that 10 uF is sufficient.

The [Espressif schematic checklist](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c6/schematic-checklist.html)
recommends bulk capacitance to address transmit-induced rail collapse.
Its chip-level bypass recommendations are not a replacement for the module
reference circuit or a completed carrier power-distribution analysis.
For the final regulator and load switch, evaluate the current deficit and
effective capacitance together:

```text
DeltaQ = integral(Iload(t) - Isource(t), dt)
DeltaV_capacitive = DeltaQ / C_effective
```

ESR, interconnect impedance and regulator response add further effects.
No current waveform, response interval or allowable transient budget has
yet been established for this rail, so a numerical droop sign-off would
be unsupported. The complete waveform must remain within 3.0..3.6 V at
the module supply contact. U4 now provides the load-switch output connection;
its VIN now connects to +3V3_MCU with C47 input bypass. The upstream regulator
and switch controls remain unimplemented. There is deliberately
no PWR_FLAG on this rail.

Reproduce the arithmetic from the repository root (read-only):

```sh
python3 - <<'PY'
import csv
from fractions import Fraction as F

path = 'ra8p1_kicad/resources/datasheets/TDK_C3216X7R1V106K160AC_dc_bias_2026-09-05.csv'
with open(path, newline='') as stream:
    rows = list(csv.reader(stream))
assert rows[5] == ['C3216X7R1V106K160AC']
assert rows[6] == ['DC/V', 'Capacitance(Nom.)/F']
curve = {F(row[0]): F(row[1]) * 1000000 for row in rows[7:] if len(row) == 2}
va, vb = F('3.15'), F('4')
ca, cb = curve[va], curve[vb]
assert ca == F('9.69875') and cb == F('9.38950')
for voltage, expected in ((F('3.3'), F(327902, 34000)),
                          (F('3.6'), F(324191, 34000))):
    capacitance = ca + (voltage - va) / (vb - va) * (cb - ca)
    assert capacitance == expected
    print(f'{float(voltage):.1f} V: {float(capacitance):.8f} uF nominal')
print('RADIO-003 PASS: curve interpolation only; transient behavior unverified.')
PY
```

## Module sourcing

U3's native KiCad fields identify Espressif Systems ESP32-C6-WROOM-1-N8
and [DigiKey 1965-ESP32-C6-WROOM-1-N8CT-ND](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-WROOM-1-N8/17728866).
The 2026-09-05 listing showed Active status, 1435 units in stock, and USD
5.55 / 4.80 / 4.1844 at quantities 1 / 10 / 100. A US tariff may apply.
This is a dated procurement snapshot, not a stock reservation or design
qualification. Recheck availability and total price before ordering.

## RADIO-004: radio load switch

Revision 1, 2026-09-05. Applies to U4 on
[the radio sheet](../ereader/radio_esp32.kicad_sch), with the same RADIO-004
identifier on the schematic. This is a partial implementation, not electrical
release: VIN is connected to +3V3_MCU with C47 input bypass; ON and CT
require completion with the upstream regulator,
reset supervisor and signal isolation under issues #825 and #826.

U4 is Texas Instruments TPS22917DBVT, the active-high version. The exact
[TI datasheet](https://www.ti.com/lit/ds/symlink/tps22917.pdf),
SLVSDW8B Rev. B, December 2021, is the electrical authority. Table 6-1,
page 4, gives pins 1 VIN, 2 GND, 3 ON, 4 CT, 5 QOD and 6 VOUT.
The project-local symbol is copied through KiCad from the bundled
Power_Management:TPS22917DBV symbol, with exact ordering and sourcing fields.
Its 100 mil pins retain the compact standard-symbol geometry, as with U2;
the placed reference/value are automatically stacked above the body.
No existing footprint or PCB geometry was changed or qualified.

QOD is modeled as a passive analog discharge terminal, not a logic
open-collector output: it exposes the switched internal 150 ohm typical
discharge path and is explicitly intended to connect to VOUT. Keeping the
bundled logic-output type produces a false conflict with VOUT's power-output
type for TI's recommended direct connection. Only U4's project-local pin 5
type was corrected; no ERC matrix rule or exclusion was changed. VIN/GND
remain power inputs, VOUT a power output, ON an input and CT an output.

Native fields identify [DigiKey 296-48370-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/TPS22917DBVT/8567084).
The listing inspected on 2026-09-05 showed Active status, 8127 units and USD
1.14 / 0.822 / 0.6554 at quantities 1 / 10 / 100. Availability is not reserved.
The DBVR listing is out of stock; do not report its DBVT substitute inventory
as DBVR inventory. TI's packaging table identifies DBVT as a 250-piece small
reel. TPS22917L is active-low and is not an interchangeable control option.

### Leakage, rail drop and protection limits

Table 7.5, page 6, specifies enabled input quiescent current of 0.5 uA
typical, 1.0 uA maximum over -40..85 C, with output open. Disabled current
for TPS22917 is 10 nA typical, 100 nA maximum over -40..85 C with VOUT at
ground. These are switch-only figures; module, supervisor, isolator and
board leakage must be added to the system sleep budget.

RON is specified at discrete VIN values, not as an all-voltage 80 mohm
maximum. At VIN=3.6 V and IOUT=200 mA it is 90 mohm typical and 140 mohm
maximum over -40..85 C; at 1.8 V the corresponding maximum is 175 mohm.
Do not interpolate these maxima into a guaranteed 3.3 V specification.
The following is an explicit screening allocation, not a guaranteed bound:

```text
Assume upstream nominal 3.3 V +/-2% and allocated switch R = 0.175 ohm.
Use I = 0.5 A as a supply-capability screening point, not a measured load.
VIN_low = 3.3*(1-0.02) = 3.234 V
Vdrop = I*R = 0.5*0.175 = 0.0875 V
VOUT_screen = 3.234-0.0875 = 3.1465 V
Headroom above module 3.0 V minimum = 0.1465 V
P_switch_screen = I^2*R = 0.5^2*0.175 = 0.04375 W
```

This headroom excludes wiring, regulator transient error and other drops.
It also does not establish reset-supervisor compatibility: U2's existing
TPS3808G33 conservative release screen is 3.19395125 V (RST-001), above
this loaded rail screen by 0.04745125 V. A radio supervisor cannot simply
be copied with a claim of full-load release margin. Joint rail regulation,
load-switch loss, reset thresholds and radio load sequencing need resolution.

The switch's 2 A absolute maximum is not a current-limit function. Upstream
fault protection remains necessary. Section 9.4 and Table 7.5 describe reverse
current detection with -0.5 A typical/-1 A limit and 10 us typical activation
at the stated reverse-voltage condition. Do not call this instantaneous,
zero-reverse-current isolation. It does not isolate the ESP32 GPIOs from a
powered host; separate partial-power-down signal isolation remains required.

### Output discharge calculation

U4.5 QOD connects directly to U4.6 VOUT, which supplies +3V3_RADIO.
U4.2 connects to GND. Section 9.3.3, pages 15-16, permits this direct
connection. With no added resistor, the nominal internal discharge resistance
is 150 ohm. This is a typical value, not a characterized min/max guarantee.

```text
External nominal capacitance = C45+C46 = 0.1+10 = 10.1 uF
For an unloaded ideal RC approximation:
tau = R*C = 150*10.1e-6 = 0.001515 s = 1.515 ms
t_90_to_10 = ln(0.9/0.1)*tau = ln(9)*tau = 3.328795235 ms
TI equation 4 approximation: 2.2*R*C = 3.333 ms
```

The fitted capacitors' bias/tolerance, the module's internal capacitance,
other future radio-rail parts and actual load change this result. When VIN
collapses, discharge strength also falls (section 9.3.3.1). This estimate
therefore does not set a guaranteed minimum radio-off time or prove a reset.
Power-cycle timing must wait for the complete circuit and measured discharge.

### Timing-capacitor rule for the next circuit increment

CT connects to VIN, NOT ground, and TI specifies a capacitor voltage rating
of at least 7 V (Table 7.3). This differs from the imported TPS22918 family.
U4.4 remains visibly unwired; no timing capacitor is fitted or approved yet.

Datasheet equation 1 prints CT = slew/SRON, but its units are inconsistent.
Use equation 6 and the worked example on page 18:

```text
CT[pF] = SRON[(mV/us)*pF] / desired_slew[mV/us]
desired_slew = allowed_capacitive_inrush / total_output_capacitance
```

The error is identified by dimensional analysis and the manufacturer's own
worked example, not by silently changing the source. At the 3.6 V table
point SRON is 1900 (mV/us)*pF for CT >=100 pF. For illustration only,
CT=1000 pF would give 1.9 mV/us, and 10.1 uF would draw 19.19 mA
capacitive inrush. Total input current also includes the load. These timing
coefficients are typical; 3.6 V is a table point, not the selected rail value.
Select CT only after the upstream transient budget and complete capacitance
are known, and maintain an independent power-stable reset delay.

### Reproducible arithmetic

Run from any directory; this verifies the stated screening arithmetic only:

```sh
python3 - <<'PY'
from fractions import Fraction as F
from math import log, isclose

vin_low = F('3.3') * (1-F('0.02'))
current, resistance = F('0.5'), F('0.175')
drop = current * resistance
vout = vin_low-drop
assert vin_low == F('3.234') and drop == F('0.0875')
assert vout == F('3.1465') and vout-F(3) == F('0.1465')
assert current**2*resistance == F('0.04375')
release = F('3.07')*F('1.015')*F('1.025')
assert release-vout == F('0.04745125')
cap_u = F('10.1')
tau_ms = F(150)*cap_u/F(1000)
assert tau_ms == F('1.515')
exact_ms = log(9)*float(tau_ms)
assert isclose(exact_ms, 3.328795234664, abs_tol=1e-9)
assert F('2.2')*tau_ms == F('3.333')
slew = F(1900)/F(1000)
assert slew*cap_u == F('19.19')
print(f'RC 90..10% estimate: {exact_ms:.9f} ms')
print('RADIO-004 PASS: arithmetic only; rail/timing qualification open.')
PY
```

## RADIO-005: input bypass

Revision 1, 2026-09-05. Applies to C47 and U4 VIN on
[the radio sheet](../ereader/radio_esp32.kicad_sch). The schematic carries
the same RADIO-005 identifier and links back to this section.

C47 connects between +3V3_MCU and GND, directly at the U4 input network.
It uses TDK C3216X7R1V106K160AC, 10 uF nominal, +/-10%, 35 V X7R,
with the same exact ordering code and sourcing fields as C46. The
selection is initial local bypass, not completed regulator qualification.
[TI TPS22917 datasheet](https://www.ti.com/lit/ds/symlink/tps22917.pdf)
section 11, page 19, recommends 1 uF in most applications and additional
bulk capacitance when the source responds slowly to load steps. It also
requires the source to withstand the transient current demand. Choosing
10 uF does not remove that requirement.

The exact-part TDK curve and reproducible interpolation in
[RADIO-003](#radio-003-bulk-capacitance) also apply to C47:

```text
C47(3.3 V) = 9.69875 + (3.3-3.15)/0.85*(9.38950-9.69875)
           = 9.644176470588... uF nominal
DeltaQ = integral(Iload(t)-Isource(t), dt)
DeltaV_capacitive = DeltaQ / C47_effective
```

The capacitance is not a guaranteed minimum; initial tolerance,
temperature, aging and excitation remain outside that curve calculation.
ESR and ESL add voltage excursions, and the shared MCU rail adds other
loads and capacitances. Do not use C47 alone to characterize the complete
input power-distribution network. No numerical transient droop is claimed
without a regulator response, load waveform and acceptable rail budget.

C47 is upstream of U4, so it is deliberately excluded from RADIO-004's
switched output-discharge capacitance C45+C46 = 10.1 uF nominal. Include
C47 in the upstream regulator's eventual startup and stability analysis.
No PWR_FLAG was added: the upstream regulator is not yet implemented.

Run the RADIO-003 Python block to verify the shared exact-part curve.
The following additionally verifies the nominal capacitance accounting:

```sh
python3 - <<'PY'
from fractions import Fraction as F

caps_u = {'C45': F('0.1'), 'C46': F(10), 'C47': F(10)}
output_refs = ('C45', 'C46')
input_refs = ('C47',)
assert set(output_refs).isdisjoint(input_refs)
assert sum(caps_u[ref] for ref in output_refs) == F('10.1')
assert sum(caps_u[ref] for ref in input_refs) == F(10)
print('RADIO-005 PASS: nominal accounting only; verify nets separately.')
PY
```
