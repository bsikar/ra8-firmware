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
the module supply contact. Source circuitry remains unimplemented and
there is deliberately no PWR_FLAG on this rail.

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
