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

Full-duplex SPI is the selected schematic transport. GPIO4 is
also a strap and must not be loaded by a host pull during strap sampling.
GPIO6/7 also have JTAG functions. Keep recovery programming separate from
these reused signals. SDIO is not connected in this design. Do not label SPI
frequency as achieved throughput: framing, handshakes and firmware reduce it.

Source: [Espressif Hosted full-duplex SPI guide](https://raw.githubusercontent.com/espressif/esp-hosted-mcu/main/docs/spi_full_duplex.md),
sections 3.1 and 5, read 2026-09-05. Pin defaults are configurable and must
be frozen with the coprocessor firmware configuration before release.

### RADIO-008: RA8P1 host allocation

Revision 1, 2026-09-05. Applies to U1E on the MCU I/O sheet. The selected
host peripheral is SPIA, using its C pin mapping. Authority:
[RA8P1 datasheet R01DS0439EJ0130](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet),
Table 1.17, standard-product BGA289 column, pages 28-30. Do not use the
303-ball table or the alternate 224-ball column: their contact assignments
are different. All eight port names and ball numbers below were checked
against the project symbol before wiring the hierarchical labels.

| MCU boundary net | U1 ball | Port / selected function | MCU sheet direction |
| --- | --- | --- | --- |
| RADIO_CIPO | F12 | P700 / MISOA_C | Input |
| RADIO_COPI | F15 | P701 / MOSIA_C | Output |
| RADIO_SCLK | F13 | P702 / RSPCKA_C | Output |
| RADIO_CS_N | G14 | P703 / SSLA0_C | Output |
| RADIO_DATA_READY | G13 | P704 / IRQ26 | Input |
| RADIO_HANDSHAKE | F17 | P705 / IRQ19 | Input |
| RADIO_RESET_REQ_N | E17 | P706 / GPIO, active-low request | Output |
| RADIO_PWR_EN | F16 | P707 / GPIO, active-high request | Output |

These are host-side signals in the MCU supply domain, not permission to
short host and switched-radio domains together. RESET_REQ_N is a request
to the pending reset arbitration circuit; it is not a direct EN connection.
The parent sheet connects P707 to R7 through RADIO_PWR_EN. The other seven
parent-sheet connections and radio-domain isolation remain incomplete.
The generic multi-function GPIO symbol pins still use the imported passive
ERC type; hierarchical directions do not replace final pin-type review.

This allocation consumes neither SDRAM/ExBus signals nor OSPI signals in
Table 1.17. It leaves SPIB for other peripherals and preserves the P100-P104
OSPI0 group. It excludes Ethernet on these pins, SD1 eight-bit data pins
4-7 in mapping B, and the overlapping parallel camera functions. P706/P707
cannot also serve USBHS overcurrent inputs; the planned USB device/charging
interface must not silently repurpose them for USB host mode. P703's IRQ19
alternate is disabled: IRQ19 belongs to P705 only. IRQ26 and IRQ19 are
ordinary interrupt inputs, not a claim of deep-standby wake capability.

Firmware must implement the non-Espressif ESP-Hosted port, configure the
selected SPIA mux and two distinct IRQ inputs, and keep CS inactive between
transfers. A host transaction requires HANDSHAKE asserted; DATA_READY alone
is not readiness. Start evaluation at 5 MHz per the Espressif guide, then
qualify timing through the selected isolation components before increasing
speed. At 5 MHz, a 1600-byte transfer takes at least 1600*8/5e6 = 2.56 ms;
the per-direction raw ceiling is 5e6/8 = 625000 bytes/s. Protocol overhead,
software scheduling and handshake gaps reduce payload throughput. This is
not a demonstrated throughput figure or a completed application budget.

Reproduce this arithmetic (no schematic changes):

```sh
python3 - <<'PY'
from fractions import Fraction as F
clock_hz = 5000000
frame_bytes = 1600
assert F(frame_bytes*8, clock_hz) == F('0.00256')
assert F(clock_hz, 8) == 625000
print('RADIO-008 PASS: 2.56 ms/frame and 625000 bytes/s raw; not measured throughput.')
PY
```

Boot and power controls must establish safe hardware defaults before the
MCU firmware runs. In particular, disable MCU pull-ups on the enable divider
and on C6 strap-related paths. Signal isolation, power-good qualification,
reset hold and recovery access remain required under RADIO-002 and #825/#826.

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

The module rail is named +3V3_RADIO; U4 provides its load-switch source,
but the upstream regulator and complete enable/control policy remain open.
No PWR_FLAG declares the unfinished upstream supply driven. Every
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
remains unimplemented. R7/R8 now connect ON to a default-off enable divider,
but its host GPIO and timing control are not yet qualified. There is deliberately
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
release: VIN is connected to +3V3_MCU with C47 input bypass; ON has the
RADIO-006 divider and C48 connects CT to VIN. Host control requires completion
with the upstream regulator,
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

### Timing-capacitor connection and calculation authority

CT connects to VIN, NOT ground, and TI specifies a capacitor voltage rating
of at least 7 V (Table 7.3). This differs from the imported TPS22918 family.
C48 is now fitted between U4.4 and VIN. Its exact-part selection and initial
timing screen are documented in [RADIO-007](#radio-007-slew-rate-capacitor).

Datasheet equation 1 prints CT = slew/SRON, but its units are inconsistent.
Use equation 6 and the worked example on page 18:

```text
CT[pF] = SRON[(mV/us)*pF] / desired_slew[mV/us]
desired_slew = allowed_capacitive_inrush / total_output_capacitance
```

The error is identified by dimensional analysis and the manufacturer's own
worked example, not by silently changing the source. At the 3.6 V table
point SRON is 1900 (mV/us)*pF for CT >=100 pF. The initial fitted
CT=1000 pF gives a 1.9 mV/us estimate, and 10.1 uF would draw 19.19 mA
capacitive inrush. Total input current also includes the load. These timing
coefficients are typical; 3.6 V is a table point, not the selected rail value.
Finalize CT after the upstream transient budget and complete capacitance
are known, and maintain an independent power-stable reset delay. Its present
value is an initial implementation, not completed startup qualification.

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

## RADIO-006: enable divider

Revision 1, 2026-09-05. R7 is the series resistor from hierarchical input
RADIO_PWR_EN to U4 ON; R8 connects ON to GND. The radio sheet and its parent
have matching input pins. The parent input remains unrouted pending host
GPIO allocation; this is not a completed host interface.

Both resistors are YAGEO RC0603FR-0710KL, 10 kohm +/-1%, 100 mW at 70 C,
with +/-100 ppm/C temperature coefficient. Exact part and procurement fields
are retained from the already sourced MCU resistors; copied MCU pull-up
rationale is replaced with RADIO-006 rationale. Their sourcing snapshot is
dated, not a reservation. Footprints remain deferred.

### Why a divider, not just a pulldown

[TI TPS22917 datasheet](https://www.ti.com/lit/ds/symlink/tps22917.pdf),
Table 7.3, requires ON <=0.35 V for low and >=1.0 V for high. The
[RA8P1 datasheet](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet),
R01DS0439EJ0130 Rev.1.30, Table 2.7, page 56, specifies general output
VOL <=0.5 V at 1 mA and VOH >=VCC-0.5 V at -1 mA. Therefore a direct
connection cannot be signed off from those GPIO limits alone. The divider
attenuates the low level while retaining adequate high-level voltage.

The following calculation assumes a host supply of at least 3.0 V, hence
VOH >=2.5 V for the stated GPIO category. It is not approval of an arbitrary
GPIO or drive-strength setting. The final allocation must satisfy the exact
pin's electrical table, current limit, reset state and power-domain behavior.

### Resistance and leakage screening

Use an explicit +/-2% resistance envelope for screening. Initial +/-1% and
100 ppm/C over -40..85 C relative to 25 C give a maximum 65 C offset:

```text
Rmax_screen = 10000*1.02 = 10200 ohm
Rmin_screen = 10000*0.98 = 9800 ohm
Initial + temperature upper factor = 1.01*(1+65*100e-6) = 1.016565
Initial + temperature lower factor = 0.99*(1-65*100e-6) = 0.983565
```

Thus +/-2% covers those initial and temperature terms, but is not a full
life/aging or solder-stress guarantee. Include those effects at final review.

Allocate +/-10 uA total current injected into the ON node for screening.
This is an allocation, NOT a measured board leakage or an all-state TI limit.
TI Table 7.5 specifies +/-10 nA ON leakage in the enabled state only. The
RA8P1 Table 2.7, page 57, gives up to 5 uA three-state leakage for 5 V-tolerant
ports and 1 uA for other listed ports under its stated powered conditions.
Neither table alone establishes behavior with the host unpowered. The final
design must verify all power states and board leakage against the allocation.

For R7 series, R8 shunt, and signed injected current Ileak:

```text
VON = VGPIO * R8/(R7+R8) + Ileak * (R7*R8)/(R7+R8)
Divider fraction range = 9800/(10200+9800)..10200/(9800+10200)
                       = 0.49..0.51
Rparallel <=10200/2 = 5100 ohm
VON_low <=0.5*0.51 + 10e-6*5100 = 0.306 V
Low margin >=0.35-0.306 = 0.044 V
VON_high >=2.5*0.49 - 10e-6*5100 = 1.174 V
High margin >=1.174-1.0 = 0.174 V
Host Hi-Z: VON <=10e-6*10200 = 0.102 V
```

These are conservative separated extrema; they do not depend on pretending
all extrema occur at one resistor corner. The internal 750 kohm typical
smart pulldown is omitted from this screen. TI disconnects it after ON is
driven high, so it is not relied on to discharge a subsequently floating host
control. R8 supplies the external default-off path. Disable any host internal
pull-up: the RA8P1's up-to-300 uA pull-up would exceed the leakage allocation.

### Current, dissipation and timing

```text
At VGPIO <=3.6 V, maximum divider current without node leakage:
I <=3.6/(9800+9800) = 183.673469 uA
Conservative host-current bound including the whole leakage allocation:
Ihost <=183.673469+10 = 193.673469 uA <1 mA
Ptotal <=3.6^2/19600 = 0.661224490 mW (zero-leakage screen)
Nominal enabled current at 3.3 V = 3.3/20000 = 165 uA
```

Do not report the load switch's sub-microamp quiescent current as the whole
enabled control budget: the divider draws the current above while enabled.
The 100 mW resistor rating is ample for this screening load, subject to the
manufacturer's temperature derating and final stress review.

With the host Hi-Z and no injected leakage, ideal discharge from 1.65 V to
0.35 V is t=R8*Cnode*ln(1.65/0.35). Cnode and all-state leakage are not yet
qualified, so this does not establish a numerical reset or power-off delay.
U4 CT timing, the radio supervisor, output discharge and GPIO isolation are separate
circuits; this divider does not replace them or prevent signal back-powering.

### Reproducible arithmetic

```sh
python3 - <<'PY'
from fractions import Fraction as F
from itertools import product

rlo, rhi = F(9800), F(10200)
leak = F('0.000010')
assert F('1.01')*(1+F(65)*F('0.0001')) == F('1.016565') < F('1.02')
assert F('0.99')*(1-F(65)*F('0.0001')) == F('0.983565') > F('0.98')
assert rlo/(rlo+rhi) == F('0.49')
assert rhi/(rlo+rhi) == F('0.51')
vlow = F('0.5')*F('0.51')+leak*rhi/2
vhigh = F('2.5')*F('0.49')-leak*rhi/2
assert vlow == F('0.306') < F('0.35')
assert vhigh == F('1.174') > F(1)
assert leak*rhi == F('0.102')
for rs, rp, il in product((rlo,rhi),(rlo,rhi),(-leak,leak)):
    parallel = rs*rp/(rs+rp)
    assert F('0.5')*rp/(rs+rp)+il*parallel <= vlow
    assert F('2.5')*rp/(rs+rp)+il*parallel >= vhigh
current = F('3.6')/(2*rlo)
assert current+leak < F('0.001')
assert F('3.3')/20000 == F('0.000165')
print(f'Divider current bound: {float(current)*1e6:.6f} uA before leakage')
print(f'Divider power bound: {float(F("3.6")**2/(2*rlo))*1000:.9f} mW')
print('RADIO-006 PASS: screening arithmetic; host/power-state qualification open.')
PY
```

## RADIO-007: slew-rate capacitor

Revision 1, 2026-09-05. C48 on the radio sheet is the U4 timing capacitor,
connected between CT and VIN, not GND. Its schematic annotation links to this
section. C48 is not an output bypass capacitor and is not added to the
RADIO-004 output-discharge capacitance.

### Exact part and sourcing

C48 is [TDK C1608NP01H102J080AA](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1608NP01H102J080AA),
1 nF +/-5%, 50 V, NP0 with 0 +/-30 ppm/C temperature characteristic.
TDK lists Production status and -55..150 C operation. This stable dielectric
is appropriate for an initial timing component; its 50 V rating exceeds
TI's 7 V minimum timing-capacitor rating in Table 7.3. Do not substitute a
6.3 V part merely because the external rail is nominally 3.3 V.

[DigiKey 445-14055-1-ND](https://www.digikey.com/en/products/detail/tdk/C1608NP01H102J080AA/3955721)
was listed Active on 2026-09-05 with 27,277 units, 24-week standard lead
time and USD 0.22 / 0.125 / 0.078 at quantities 1 / 10 / 100. Recheck stock,
price and any tariff before ordering. These exact values are recorded in
C48's native sourcing fields. No footprint geometry was added or qualified.

### Initial timing and capacitive-inrush screen

The authority is [TI TPS22917 datasheet](https://www.ti.com/lit/ds/symlink/tps22917.pdf)
SLVSDW8B Rev. B, Table 7.6, page 7, and section 10.2.2.1, page 18.
Use the consistent equation 6, CT=SRON/slew; RADIO-004 explains the
dimensionally inconsistent printed equation 1. The 1 nF initial choice slows
the output edge substantially relative to open CT while retaining a
millisecond-scale typical turn-on. It does not establish the final current
budget or reset-release delay.

At the 3.6 V table point, 25 C, using TI's typical coefficients:

```text
CT = 1 nF = 1000 pF
slew = 1900 [(mV/us)*pF] / 1000 [pF] = 1.9 mV/us = 1900 V/s
tON = 3.8 [us/pF] * 1000 [pF] = 3800 us = 3.8 ms
tR = 1.6 [us/pF] * 1000 [pF] = 1600 us = 1.6 ms
Cexternal_nominal = C45+C46 = 10.1 uF
Icapacitive = C*dV/dt = 10.1e-6*1900 = 0.01919 A = 19.19 mA
Open CT comparison: Icapacitive = 10.1e-6*44000 = 0.4444 A
```

tR is TI's 10..90% output rise-time metric; tON also includes turn-on delay.
These are separately tabulated typical metrics, not exact interchangeable
linear-ramp definitions. Table 7.6 uses CL=1 uF and RL=10 ohm unless
otherwise stated. The external 10.1 uF calculation above is a capacitor-only
estimate, not a repetition of TI's test load. Neither the 3.6 V coefficients
nor the resulting times are guaranteed at the selected 3.3 V rail.

Module internal capacitance, the reset supervisor and other future radio
components add load. Operating current adds to capacitive current. Current
drawn while the module rail ramps must be evaluated with the final EN/reset
network; 19.19 mA is not the full module startup current. U4 is not a current
limiter, and its 2 A absolute maximum must not be used as a protection setting.

### Capacitor tolerance sensitivity, not IC timing limits

For an initial -40..85 C screen, maximum offset from 25 C is 65 C:

```text
Cmin = 1000*(1-0.05)*(1-65*30e-6) = 948.1475 pF
Cmax = 1000*(1+0.05)*(1+65*30e-6) = 1052.0475 pF
```

Substituting these endpoints into the typical coefficients illustrates
capacitor sensitivity only. It does not create minimum/maximum limits for
TI's typical slew or delay coefficients. Manufacturing/temperature variation
of the IC, additional capacitor effects, rail regulation, parasitics and
actual startup loading still require qualification. Keep an independent
power-stable reset delay; do not release module EN solely after a fixed
3.8 ms firmware wait.

### Reproducible arithmetic

```sh
python3 - <<'PY'
from fractions import Fraction as F

ct_pf = F(1000)
sr_coefficient = F(1900)
slew_mv_us = sr_coefficient/ct_pf
assert slew_mv_us == F('1.9')
assert F('3.8')*ct_pf == F(3800)
assert F('1.6')*ct_pf == F(1600)
output_u = F('0.1')+F(10)
assert output_u*slew_mv_us == F('19.19')
assert output_u*F(44) == F('444.4')
cmin = ct_pf*F('0.95')*(1-F(65)*F('0.000030'))
cmax = ct_pf*F('1.05')*(1+F(65)*F('0.000030'))
assert cmin == F('948.1475') and cmax == F('1052.0475')
assert F(50) >= F(7)
for capacitance in (cmin, ct_pf, cmax):
    slew = sr_coefficient/capacitance
    print(f'C={float(capacitance):.4f} pF: typical-coefficient '
          f'slew={float(slew):.6f} mV/us, '
          f'tON={float(F("3.8")*capacitance/1000):.6f} ms, '
          f'Icap={float(output_u*slew):.6f} mA')
print('RADIO-007 PASS: arithmetic only; complete startup qualification open.')
PY
```

## RADIO-009: SPI power-domain isolation

Revision 1, 2026-09-05. Electrical selection record for the proposed
TXU0304PWR SPI isolator on [the radio sheet](../ereader/radio_esp32.kicad_sch).
Tracking: [issue #826](https://github.com/bsikar/ra8-firmware/issues/826).
The project-local `Power_Devices:TXU0304PWR` symbol implements the exact PW
pin map below, with explicit input/tri-state/power types and visible NC
contacts. Its 150 mil pins terminate on the 100 mil connection grid.
U5 is placed with VCCA on +3V3_MCU, VCCB on +3V3_RADIO and pin 7 on GND.
Its exact manufacturer and DigiKey ordering fields are included in the BOM.
C49 and C50 provide separate local supply bypasses. U5.13 connects to U3.6
(GPIO6, C6_SCLK), and U5.12 connects to U3.7 (GPIO7, C6_COPI).
U5.11 connects to U3.11 (GPIO10, C6_CS_N); U3.27 (GPIO2, C6_CIPO)
connects to U5.10. These four radio-side nets use local labels with separate
short connections, without crossing the supply domains directly.
The host SPI connections now pass through matching hierarchical ports and
straight top-sheet wires to the RADIO-008 allocation: U1.F13 (P702) to U5.2
for SCLK, U1.F15 (P701) to U5.3 for COPI, U1.G14 (P703) to U5.4 for CS_N,
and U5.5 to U1.F12 (P700) for CIPO. Host and C6 signal nets remain separate
on opposite sides of U5. Power enable remains U1.F16 (P707) to R7.1.
OE wiring, reset arbitration and the handshake/data-ready interfaces remain
incomplete; this is not a qualified circuit. The RADIO-009 schematic
annotation links to this section.

### Device and channel assignment

The authority is [TI TXU0304 datasheet](https://www.ti.com/lit/ds/symlink/txu0304.pdf),
SCES935A, sections 6, 7.5, 7.11, 8.1, 9.3 and 12.1. The PW package has
14 pins, including visible NC contacts 6 and 9; it has no exposed-pad pin.
This is a fixed-direction buffer, not an automatic-direction TXB device.

| PW contact | Function | Proposed connection |
| --- | --- | --- |
| 1 | VCCA | +3V3_MCU |
| 14 | VCCB | +3V3_RADIO |
| 7 | GND | Common GND |
| 2 -> 13 | A1 -> B1Y | Host SCLK -> C6 SCLK |
| 3 -> 12 | A2 -> B2Y | Host COPI -> C6 COPI |
| 4 -> 11 | A3 -> B3Y | Host CS_N -> C6 CS_N |
| 10 -> 5 | B4 -> A4Y | C6 CIPO -> host CIPO |
| 8 | OE, active high | Default-low interface enable; arbitration unresolved |
| 6, 9 | NC | Unconnected, visible in symbol |

Provide separate local 100 nF bypass capacitors from each supply to GND.
Do not bridge the two supplies through either capacitor. Both ports support
1.1..5.5 V, so equal nominal 3.3 V rails are permitted. This device provides
power-domain separation, not galvanic isolation.

C49 and C50 use TDK C1608X7R1H104K080AA, 100 nF, +/-10%, 50 V X7R,
the same sourced ordering code as C45. C49 connects VCCA to GND; C50
connects VCCB to GND. The choice implements TI section 12.1's 100 nF local
bypass recommendation; it is not derived from the SPI bit period. Keep each
capacitor's eventual supply/return loop local to its corresponding IC pins.
Nominal capacitance, voltage rating and matching an application recommendation
do not establish transient performance or the finished power budget.

### Updated switched-rail capacitive-load accounting

C50 adds to the switched output load; C49 is on the upstream MCU domain and
does not. The earlier RADIO-004/RADIO-007 calculations record the C45+C46
population at those revisions. With C50 now fitted, update their capacitor-only
screens as follows, retaining all their stated limitations:

```text
Cexternal_nominal = C45 + C46 + C50 = 0.1 + 10 + 0.1 = 10.2 uF
t90_to_10 = Rdis * C * ln(9)
          = 150 ohm * 10.2e-6 F * ln(9) = 3.361754 ms typical-coefficient model
Icapacitive = C * slew = 10.2e-6 F * 1900 V/s = 19.38 mA
Open-CT comparison = 10.2e-6 F * 44000 V/s = 448.8 mA
```

These are not worst-case bounds. They exclude module internal capacitance,
IC operating current and future fitted components. The discharge screen uses
typical QOD resistance; the slew screen uses the earlier 3.6 V/25 C typical
coefficient rather than a guaranteed 3.3 V ramp. No fixed reset-release delay
may be inferred from either result.

### Conditional round-trip SPI timing budget

At VCCA=VCCB=3.3 +/-0.3 V, maximum propagation delay is 11 ns in each
direction over -40..125 C. TI specifies this with 5 pF load and 10 kohm
test resistance. A larger actual load is not covered by this timing claim.

For an initial 5 MHz SPI clock with opposite-edge launch and sample:

```text
T = 1/(5e6 Hz) = 200 ns
Half-cycle = T/2 = 100 ns
Translator round trip = 11 ns outbound SCLK + 11 ns inbound CIPO = 22 ns
Remaining budget = 100 - 22 = 78 ns

Required:
C6 clock-to-output + RA8P1 setup + trace/skew allowance < 78 ns
```

This is a conditional budget, NOT timing closure or a 5 MHz guarantee.
The chosen SPI mode, C6 clock-to-output limit, RA8P1 input setup requirement,
duty-cycle distortion, output loading and interconnect delay must satisfy
the inequality together. Frequency reduction does not resolve an invalid
logic threshold, off-state path or reset sequence.

### Power-off and enable constraints

For one supply at 0 V and the other within 0..5.5 V, TI specifies Ioff of
+/-2 uA per port contact over -40..85 C and +/-2.5 uA over -40..125 C,
with signal voltages in 0..5.5 V. The separate floating-supply leakage test
uses signals at GND; do not extend that test to a driven-high floating rail.

OE low disables all outputs. At the same 3.3 V timing test point, allow
42 ns maximum disable time over -40..125 C before treating outputs as
high impedance. This is a propagation limit under TI's stated test load,
not a complete rail-collapse or firmware sequencing allowance.

Use an external default-low OE network and establish valid power/reset
conditions before enabling. Do not connect OE directly to RADIO_PWR_EN and
assume that the load-switch command proves stable radio power. The C6-side
CS_N idle level and host-side CIPO idle level need explicit handling while
the buffer is disabled. GPIO handshakes, reset and debug paths require their
own off-state review; isolating these four SPI channels does not isolate
the complete module.

### Reproducible arithmetic

```sh
python3 - <<'PY'
from fractions import Fraction as F

frequency_hz = F(5_000_000)
period_ns = F(1_000_000_000)/frequency_hz
half_cycle_ns = period_ns/2
outbound_ns = F(11)
inbound_ns = F(11)
remaining_ns = half_cycle_ns-outbound_ns-inbound_ns
assert period_ns == 200 and half_cycle_ns == 100
assert outbound_ns+inbound_ns == 22
assert remaining_ns == 78
external_uf = F('0.1')+F(10)+F('0.1')
assert external_uf == F('10.2')
capacitive_ma = external_uf*F(1900)/1000
assert capacitive_ma == F('19.38')
assert external_uf*F(44000)/1000 == F('448.8')
from math import isclose, log
discharge_ms = 150*float(external_uf)*1e-6*log(9)*1000
assert isclose(discharge_ms, 3.3617536033244164, rel_tol=1e-12)
print(f'RADIO-009: {float(external_uf):.1f} uF nominal external switched load; '
      f'{discharge_ms:.6f} ms typical-model discharge; '
      f'{float(capacitive_ma):.2f} mA capacitor-only ramp current.')
print('RADIO-009 PASS: 78 ns conditional remaining budget; timing not closed.')
PY
```

## RADIO-010: Idle-state bias

Applies to R9, R10, U5.8 (OE) and U3.11 (C6_CS_N) on
[the radio schematic](../ereader/radio_esp32.kicad_sch).
The schematic annotation links to this section. Tracking: issue #826.

R9 pulls translator OE to ground when no enable driver is active. R10
pulls chip select to the switched +3V3_RADIO supply when U5 is disabled.
Connecting R10 to +3V3_MCU instead would create an unwanted powered-to-off
path. Both are YAGEO RC0603FR-0710KL, DigiKey 311-10.0KHRCT-ND.

### Inputs and limits

- The [exact resistor specification](https://yageogroup.com/component-documentation/download/specsheet/RC0603FR-0710KL)
  gives 10 kohm, 1% tolerance, 100 ppm/C maximum TCR magnitude and 0.1 W
  rating at 70 C. Use a conservative 100 C excursion from 25 C for this
  resistance screen; this is not an extension of the module temperature rating.
- [TI TXU0304, section 7.5, p.7](https://www.ti.com/lit/ds/symlink/txu0304.pdf)
  gives minimum OE falling threshold VT- = 0.17 V at VCCA=VCCB=1.1 V.
  This is the lowest listed equal-rail test point, not a continuous ramp
  guarantee. OE is a Schmitt input; a generic CMOS VIL must not replace VT-.
- [Espressif module datasheet v1.4, Table 6-3, p.26](https://www.espressif.com/sites/default/files/documentation/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf)
  specifies VIH = 0.75*VDD minimum at 3.3 V and 25 C. The calculation below
  does not extrapolate that table across temperature or the whole supply range.
- Total adverse node leakage is allocated 12 uA, including future control
  circuitry and board leakage. This is an engineering budget, NOT a measured
  or manufacturer-guaranteed aggregate. The final driver and all power states
  must demonstrate compliance. Disable a conflicting C6 internal pull-down;
  a configured output driving low is not leakage.

### Resistance and static voltage calculations

Use multiplicative initial tolerance and temperature factors:

```text
Temperature factor = 100 ppm/C * 100 C = 0.01
Rmin = 10000*(1-0.01)*(1-0.01) = 9801 ohm
Rmax = 10000*(1+0.01)*(1+0.01) = 10201 ohm

R9 worst allocated default-low voltage:
VOE = Iadverse*Rmax = 12e-6*10201 = 0.122412 V
Margin to the listed 1.1 V test-point VT- = 0.17-0.122412 = 0.047588 V

R10 worst allocated idle voltage at 3.3 V:
VCS = Vradio-Iadverse*Rmax = 3.3-0.122412 = 3.177588 V
VIH = 0.75*3.3 = 2.475 V
Static high margin = 3.177588-2.475 = 0.702588 V
```

These are conditional static screens. They do not establish startup,
brownout, unequal-rail transient behavior, output-enable timing or SPI
timing closure. R9 currently holds OE disabled; an active enable driver
and its power/reset arbitration are still required for operation.

### Current and power tradeoff

```text
Nominal R9 current with OE driven to 3.3 V = 3.3/10000 = 0.330 mA
Nominal R10 current with CS driven to 0 V = 3.3/10000 = 0.330 mA
Maximum screen current with 3.6 V across either resistor = 3.6/9801
  = 0.367309458 mA
Maximum screen dissipation = 3.6^2/9801 = 1.322314050 mW
```

R9's enabled current belongs in the active-radio budget. R10 draws this
current while CS is low, not while it is idle high. Actual current depends
on driver VOH/VOL. The power comparison uses the 70 C rating only; thermal
derating is not qualified here. No RC settling time is asserted because
the actual node capacitance and final driver have not been established.

### Reproducible arithmetic

```sh
python3 - <<'PY'
from fractions import Fraction as F
from math import isclose

r_nom = F(10000)
tolerance = F('0.01')
temperature_factor = F(100, 1_000_000)*100
r_min = r_nom*(1-tolerance)*(1-temperature_factor)
r_max = r_nom*(1+tolerance)*(1+temperature_factor)
assert (r_min, r_max) == (9801, 10201)
leakage_allocation = F(12, 1_000_000)
oe_low = leakage_allocation*r_max
assert oe_low == F('0.122412')
assert F('0.17')-oe_low == F('0.047588')
cs_high = F('3.3')-oe_low
vih = F('0.75')*F('3.3')
assert cs_high == F('3.177588') and vih == F('2.475')
assert cs_high-vih == F('0.702588')
assert F('3.3')/r_nom*1000 == F('0.330')
current_ma = F('3.6')/r_min*1000
power_mw = F('3.6')**2/r_min*1000
assert isclose(float(current_ma), 0.367309458, abs_tol=1e-9)
assert isclose(float(power_mw), 1.322314050, abs_tol=1e-9)
print('RADIO-010 PASS: conditional static arithmetic only; '
      '12 uA allocation, enable logic and transient qualification remain open.')
PY
```
