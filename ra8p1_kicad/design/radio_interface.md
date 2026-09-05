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
