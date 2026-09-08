# Production service interface

Revision 1, 2026-09-07. Implementation contract for issues #824/#826;
not a claim that these additions are drawn, netlist-verified or qualified.
Retain one user power button. Programming uses protected copper contacts
and a fixture, not three additional user buttons. Read alongside
[boot/reset](boot_and_debug.md) and [radio circuitry](radio_interface.md).
Those records describe earlier SW1/SW2/SW3 revisions; the following contact
contract replaces their mechanical-button interpretation only.

## SERVICE-001: existing contacts and RA8P1 recovery

Each TP below is a TWO-contact copper pattern, normally open. Contact 1 is
the signal and contact 2 GND. Neither is a populated switch or a permanent
short. Keep the existing bias resistors; do not attach a pull-up from a
fixture supply to these signals. Fixture controls are open-drain sinks.

| Pattern | Contact 1 | Contact 2 | Function |
| --- | --- | --- | --- |
| TP1, replaces SW1 | SW_RESET_N, U2 MR pin 3 | GND | Delayed supervisor reset request |
| TP2, replaces SW2 | MCU_BOOT_MD, U1 P201/MD | GND | Select serial boot while RES is cycled |
| TP3, replaces SW3 | C6_BOOT_N, U3 contact 15/GPIO9 | GND | Select C6 download boot |

TP1 is NOT the direct MCU RES net. Use J1.10/MCU_RESET_N for the fixture's
controlled reset hold. Do not connect U2 MR to its RESET output. Keep the
no-capacitor RES implementation and existing supervisor release delay.

The selected 289-ball RA8P1 reuses two existing J1 contacts for SCI9 ROM
programming. No second MCU UART header or new MCU signal allocation is
needed. These contacts must not be driven by a debugger simultaneously.

| J1 contact | Existing debug name | SCI9 service use |
| --- | --- | --- |
| 1 | +3V3_MCU | Target voltage sense only; never fixture power injection |
| 6 | DBG_TDO_SWO, U1 E7/P209 | TXD9: MCU output to fixture RX |
| 8 | DBG_TDI, U1 C7/P208 | RXD9: fixture TX to MCU input |
| 10 | MCU_RESET_N, U1 D5/RES | Open-drain fixture reset |
| 3/5/9 | GND | Common reference |

J1.2/SWDIO and J1.4/SWCLK retain their debug roles; J1.7 remains NC/key.
SCI9 and SWD/JTAG are alternative fixture personalities, not concurrent
drivers. Keep all fixture MCU-facing outputs high impedance when the target
rail is absent. The radio translator below does not protect J1.

[RA8P1 HUM](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
Rev.1.30, sections 4.3-4.4 and 60.11, Tables 60.39-60.40, establishes the
SCI9 mapping and boot restrictions. MD low plus an external RES cycle
selects SCI/USB boot; POR alone is insufficient. USB ROM boot needs USBFS
DP/DM/VBUS, not the product USBHS port. SCI remains the no-crystal fallback:
allow up to 3 s for boot connection with neither main nor subclock present.
With main clock the listed maximum is 1 s; subclock-only is 2 s. USB requires
one of those external clock sources. Do not qualify unfinished crystals.
Lifecycle, authentication, TrustZone and boot configuration can restrict
access; these pads do not guarantee recovery from every security state.

## SERVICE-002: C6 fixture contacts and power ownership

Proposed five-contact service group, logical designation J_SERVICE_C6.
Assign a physical connector/pad footprint and schematic reference only
after the fixture mechanics are selected. TP3 is separate BOOT access.

| Contact | Net | Fixture action |
| --- | --- | --- |
| 1 | GND | Make first, break last |
| 2 | SERVICE_VIO | Supply 3.3 V logic reference; also selects service mode |
| 3 | SERVICE_TX | Fixture TX, idle high, into translator A1 |
| 4 | SERVICE_RX | Fixture RX, from translator A2Y |
| 5 | SERVICE_C6_RESET_N | Open-drain reset request, never push-pull high |

SERVICE_VIO powers only the translator A domain and feeds service-selector
inputs. It does not feed +3V3_MCU, +3V3_RADIO, J1.1 or the board power tree.
Power the board through its intended system-power path. A fixture must
provide the power-latch service control specified by the power design if
the single user button cannot maintain that path. Do not bypass the radio
load switch by injecting power at its output.

This is a service-only UART, not a transparent logging connector: applying
SERVICE_VIO explicitly takes radio ownership away from the host. Normal
operation requires SERVICE_VIO absent or actively held at 0 V. A separate
presence/mode contact is unnecessary under this restricted contract.

## SERVICE-003: exact radio arbitration

Use four additional TI SN74LVC1G97DBVR devices, retaining existing U7/U8
as the normal-path generators. This is a small, single-MPN implementation
of four independently controlled outputs, not a proof of globally minimum
gate count. A quad SN74LVC157A is not substituted without separately
closing its input-edge and powered-off interface requirements.

[DigiKey 296-15581-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC1G97DBVR/571196)
checked 2026-09-07: indexed stock 30,155, USD 0.23/0.16/0.12130 at
1/10/100 units. Four gates cost USD 0.92 at the single-unit snapshot,
excluding passives, translator, tax and shipping; stock is not reserved.

[TI SCES416N](https://www.ti.com/lit/ds/symlink/sn74lvc1g97.pdf),
sections 5, 6.5 and 8.4, is the pin/truth-table authority. DBV pin 6 selects
pin 3 when high, pin 1 when low. All inputs have Schmitt behavior. Ioff
is specified at VCC=0; it is not a sub-minimum-supply logic guarantee.

Use role labels below until native schematic annotation assigns references.
EVERY added gate: pin 5 to +3V3_MCU, pin 2 to GND, pin 6 to SERVICE_VIO,
and one 100 nF local bypass from pin 5 to GND. Reuse the documented
TDK C1608X7R1H104K080AA selection. No gate output is tied to another output.

| Gate role | Pin 1 IN1: normal | Pin 3 IN0: service | Pin 4 Y destination |
| --- | --- | --- | --- |
| SERVICE_POWER | RADIO_PWR_EN host request | +3V3_MCU | RADIO_PWR_REQ_EFF -> R7 input |
| SERVICE_RESET | RADIO_MR_NORMAL_N, existing U7.4 | SERVICE_C6_RESET_N | RADIO_MR_N -> U6.3 |
| SERVICE_SPI | SPI_IO_EN_NORMAL, existing U8.4 | GND | SPI_IO_EN -> U5.8 and existing R9 |
| SERVICE_UART | GND | C6_EN | SERVICE_UART_OE -> TXU0202.6 |

Reroute R7 away from the raw host request to SERVICE_POWER.4. Preserve
R7/R8's ON divider. Rename the old U7/U8 output nets as above and insert
the new gates; do not merely add new drivers to the old nets. U8 continues
to receive the RAW host RADIO_PWR_EN request, not RADIO_PWR_REQ_EFF.
U7 retains its MCU_RESET_N and host RADIO_RESET_REQ_N inputs.

U6 remains TPS389001DSET: upstream +3V3_MCU powers VDD pin 4;
SENSE pin 1 monitors the switched radio rail; RESET pin 6 drives C6_EN.
Service reset enters MR pin 3 through the mux. Neither the undervoltage
monitor nor its delayed release is bypassed. Do not directly drive C6_EN.

Add two parallel 10k resistors from SERVICE_VIO to GND (effective 5k),
reusing YAGEO RC0603FR-0710KL. Add 10k from SERVICE_C6_RESET_N to
+3V3_MCU. SERVICE_UART_OE needs a 47k pull-down to GND. Use 47k pull-up
resistors as specified below; their exact MPN/temperature rating remains
a BOM selection, not inferred from the 10k part.

Let S mean a valid high SERVICE_VIO, P the raw host power request,
R the normal U7 output, E the qualified C6_EN, and F fixture reset release.

| S | Effective power request | U6 MR | SPI OE | UART OE |
| --- | --- | --- | --- | --- |
| 0 | P | R | P AND E | 0 |
| 1 | 1 | F | 0 | E |

The steady-state table prevents a broken host from blocking service power
or reset and prevents SPI driving during service. It is NOT a glitch-free
handover guarantee: hold both processors in reset before changing S.

## SERVICE-004: TXU0202 directions and idle bias

Select TI TXU0202DCUR, DCU VSSOP-8, requested distributor identifier
296-TXU0202DCURCT-ND; verify its current sourcing before procurement.
Pin mapping follows [TI SCES942A](https://www.ti.com/lit/ds/symlink/txu0202.pdf),
sections 6 and 9. A and B are power domains, not interchangeable UART labels.

| Pin | Name | Connection |
| --- | --- | --- |
| 1 | B2 | U3.25 TXD0/GPIO16 |
| 2 | GND | Common GND |
| 3 | VCCA | SERVICE_VIO; 100 nF local bypass |
| 4 | A2Y | SERVICE_RX, fixture input |
| 5 | A1 | SERVICE_TX, fixture output |
| 6 | OE | SERVICE_UART_OE, 47k pull-down |
| 7 | VCCB | +3V3_RADIO; 100 nF local bypass |
| 8 | B1Y | U3.24 RXD0/GPIO17 |

Proposed bias: A1 to SERVICE_VIO through 10k; B2 to +3V3_RADIO through
10k; A2Y to SERVICE_VIO through 47k; B1Y to +3V3_RADIO through 47k.
Thus disconnected/high-impedance transmitters have a local idle-high
input, and disabled translator outputs also idle high. The translator's
internal weak pull-downs alone would instead permit a UART break level.
Do not connect either external pull to the opposite power domain.

OE is active high, qualified by both adapter presence and C6_EN. Its high
level is not supplied by an absent adapter. Rail-zero isolation and Ioff
are not permission to drive arbitrary signals during brownout. The
datasheet's floating-supply condition is not equivalent to this circuit's
resistively discharged SERVICE_VIO. Validate actual disconnect waveforms.
Added radio bypass is 0.1 uF nominal; update the load-switch discharge and
startup model. Include the new C6_EN gate input in its leakage budget.

## SERVICE-005: adapter contract and conditional arithmetic

No actual adapter is selected or qualified. Require SERVICE_VIO=3.3 V
+/-5%, at least 10 mA available, and common ground. Reject RS-232 levels,
5 V TTL adapters and adapters that source board power through UART pins.
Require fixture TX VOH >= VIO-0.1 V and VOL <=0.1 V at 0.5 mA, RX input
leakage <=10 uA, RX VIH <=0.7*VIO and VIL >=0.3*VIO. Reset outputs must
sink 1 mA at <=0.1 V, leak <=10 uA when released, and tolerate a live
3.6 V target when the fixture is off. These are acceptance requirements,
not characteristics attributed to a generic USB-UART adapter.

Use the existing 1% resistor plus 100 ppm/K, 100 K arithmetic envelope:
10k = 9801..10201 ohm; 47k = 46064.7..47944.7 ohm. The latter requires
an actual resistor meeting that specification. At <=100 uA loading, use
the devices' supply-wide VOH >= VCC-0.1 and VOL <=0.1 bounds. The 47k
output pulls allow that light-load screen; a 10k output pull would not.
Allocate 12 uA total additional output load including receiver and board.
Allocate 60 uA adverse current into absent SERVICE_VIO, and 20 uA adverse
current at released fixture reset. Each is a requirement to verify.

The following checks deliberately separate supply-wide output arithmetic
from the 3 V Schmitt threshold TEST POINT. Neither TI table gives a 3.3 V
DC threshold row: interpolation is not promoted to a guaranteed limit.
Resolve full rail-range input compatibility before release. The existing
U6 MR load-current qualification remains open under RADIO-015.

```sh
python3 - <<'PY'
from fractions import Fraction as F
from itertools import product

r10lo, r10hi = F(9801), F(10201)
r47lo, r47hi = 47*r10lo/10, 47*r10hi/10
assert F('3.6')/r47lo + F('0.000012') < F('0.0001')
assert F('0.000012')*r47hi < F('.89')  # OE low, 3 V test point
assert F('0.000060')*(r10hi/2) < F('.35')  # lowest listed gate VT-
assert F('3.465')/(r10lo/2) < F('.001')  # presence pull current
reset_high = F(3)-F('.000020')*r10hi
assert reset_high > F('1.87')  # gate VT+ at 3 V only
assert F(3)-F('.1') > F('1.92')  # OE high at 3 V test point
assert F('.1') < F('.89')  # OE low at 3 V test point
assert F('3.135')-F('.1') > F('.7')*F('3.465')
assert F('.1') < F('.3')*F('3.135')  # translator -> fixture RX
assert F('3.465')/r10lo + F('.000012') < F('.0005')

def mux(s, normal, service):
    return service if s else normal

for s, p, r, e, f in product((0, 1), repeat=5):
    got = (mux(s, p, 1), mux(s, r, f), mux(s, p & e, 0), mux(s, 0, e))
    want = (1, f, 0, e) if s else (p, r, p & e, 0)
    assert got == want
print('SERVICE PASS: 32 steady states and conditional DC screens only.')
print('Adapter, intermediate-rail thresholds, ramp and timing approval open.')
PY
```

## SERVICE-006: fixture sequence and release gates

1. Connect ground; put UART TX low or high impedance until its VIO is
   valid. Assert J1.10 MCU reset and fixture C6 reset with open-drain sinks.
   Short TP3.1 to TP3.2. Keep TP2 released for C6-only service.
2. Establish intended board power and hold MCU reset throughout C6 service.
   Apply SERVICE_VIO, select idle-high TX, and wait for a valid switched
   radio rail. U6 must remain able to veto EN while its SENSE is low.
3. Release SERVICE_C6_RESET_N. Confirm supply stabilization and EN timing;
   keep GPIO9 low through at least 3 ms after EN rises, with GPIO8 high.
   Release TP3 only after the strap interval. Program through UART0.
4. For exit, assert C6 reset again, then take adapter TX low/high impedance
   and remove SERVICE_VIO. Keep MCU reset held while the selector returns
   to normal and radio power/reset settle. Release fixture C6 reset and
   MCU reset last; disconnect ground last.

For RA8 SCI service instead, use TP2 low and J1.10 held low for at least
3 ms after VCC is valid, then release RES with MD still low. Respect the
ROM connection interval and do not share J1.6/J1.8 with an attached driving
debugger. SCI fixture voltage compatibility requires its own target-side
qualification; the C6 adapter contract is not automatic approval for J1.

Before declaring implementation complete: native pin/netlist review;
independent truth-table review; exact 47k/passive/fixture sourcing;
full-range threshold and all leakage bounds; U6 MR loading; unpowered
and brownout states; mode-change glitches; supervisor/strap timing;
updated radio charge/discharge load; pad ESD and mechanical access;
and physical recovery trials with host firmware absent. Authentication
restrictions and unfinished clocks remain outside this recovery claim.
