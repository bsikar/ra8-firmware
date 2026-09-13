# microSD power and SDHI1_B implementation contract

Revision 3, 2026-09-12. Native SD power/control and six-bus interconnections
are saved, targeted XML connectivity is verified and the native exported
BOM is reconciled. Hardware qualification remains open. The role names
below now map to native references. This record extends
[CMS-003 and CMS-012](camera_storage_interfaces.md).

## Native integration checkpoint

The working export `C:/work/ereader-sd-working.xml` passed independent
review of 24 exact node assertions. U17 is the TPS22950C switch, U18 the
G30 supervisor, U19/U20 the power/IO gates and U21/U22 the bus isolators.
The six host paths use R83/R84/R88/R87/R86/R85 for CLK/CMD/DAT0..3,
respectively. Each resistor pin 1 joins its assigned MCU ball; pin 2 joins
only the assigned mux source pin. Card-side mux outputs connect to the
socket and clamp, with pullups only on CMD/DAT. The native card-side nets
are automatically named; the SD_*_CARD names below describe their roles.

Verified controls are SD_PWR_REQ={U1.N6,U19.3,R78.1},
SD_IO_REQ={U1.N12,U20.3,R79.1} and
SD_READY={U1.R17,U17.6,U18.1,U20.6,R81.2}. The common reset has seven
endpoints including U19.6; mechanical CD retains U1.F14/R73.2 and its
separate socket contact circuit. These checks cover connectivity, not
firmware initialization or operational timing.

After native layout and the FLT SD_READY label were corrected, a fresh
XML export passed the same 24 assertions and the NOR/reset regression
review. Native and CLI ERC both report 123 errors and two warnings: 16
baseline unconnected-pin errors removed, no new findings. All 125 retained
findings and four ignored checks are unchanged. CMS-012 now states that
SD power/data are integrated and retains its qualification limits; its
text was enlarged and moved clear of the circuit. Linked SD-001, SD-002
and SD-003 calculation notes are placed and independently reviewed.
The complete 12-page PDF was exported and visually reviewed, including
the corrected final microSD page. The native 19-column BOM has 94 groups
and 249 unique included references, with TP1-TP3 excluded. All values,
MPNs and grouped metadata match the XML export. Relevant SD, reset and
clock arithmetic checks pass. The project is not ERC-clean; these checks
do not establish startup, fault-slew, firmware or hardware qualification.

The working XML confirms seven local 100n parts: C48, C97, C98 and
C104-C107. C104 is on VDD_SD; the other six are on +3V3_MCU. With C102
10u and C103 22u, these give the required 10.6uF direct-main and 22.1uF
card-side nominal additions. Effective capacitance and inserted-card load
remain conditional. C48 is a reused reference for an SD bypass, not the
removed radio CT part.

## Architecture and operating envelope

Use the existing +3V3_MCU regulator with a current-limited switched VDD_SD
island and six bilateral bus isolation channels. No separate SD regulator
is selected. The prior 1.800 A main allocation excluded SD; it is neither
a measured load nor the TPS63806's validated output limit. This contract
raises the continuous allocation to 2.075 A unless the existing logic
allocation already covers the RADIO-019 5 mA support allowance, in which
case it is 2.070 A. Both cases require the source and thermal checks below.
Preserve local music, camera capture and radio use;
do not silently resolve power uncertainty by removing their concurrency.

Use removable SD memory in 3.3 V Default Speed (up to 25 MHz) and, after SI
acceptance, High Speed (up to 50 MHz). Initialize at <=400 kHz. No 1.8 V
voltage switch, UHS, SD Express, SDIO accessory, or higher power class is
supported by this circuit. The SDA table lists 0.72 W for High Speed;
Default Speed is 0.36 W, with the SDXC/SDUC XPC-dependent 0.54 W exception.
Allocate 250 mA to the card plus 20 mA to added support, including the
bleeder. This is a use-case allocation, not a guarantee about arbitrary
cards or their startup pulses. Check card mode/current declarations and
qualify actual card models. [SDA physical-layer specification v7.10,
bus-speed table](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver7.10.pdf).

## Exact host and card bus contract

Each row is U1 -> 33 ohm series resistor -> TMUX S pin -> TMUX D pin ->
card-side net -> J2. Both directions pass through the same resistor and
switch; the S/D naming does not constrain signal direction.

| Host signal / port / BGA289 ball | Isolator S / D pins | Card-side net / J2 pin |
| --- | --- | --- |
| SD1CLK_B / P400 / P17 | U21 2 / 3 | SD_CLK_CARD / 5 |
| SD1CMD_B / P401 / N17 | U21 5 / 6 | SD_CMD_CARD / 3 |
| SD1DAT0_B / P402 / L14 | U21 10 / 9 | SD_DAT0_CARD / 7 |
| SD1DAT1_B / P403 / H13 | U21 14 / 13 | SD_DAT1_CARD / 8 |
| SD1DAT2_B / P404 / J13 | U22 2 / 3 | SD_DAT2_CARD / 1 |
| SD1DAT3_B / P405 / G12 | U22 5 / 6 | SD_DAT3_CARD / 2 |

U21 and U22 are **TMUX1511RSVR**, TI RSV 16-pin UQFN,
2.6 x 1.8 mm, not TMUX1511PWR's 14-pin TSSOP footprint. On both parts:
pin 16 VDD -> +3V3_MCU, pin 8 GND -> GND, pins 7 and 12 NC -> explicit
no-connect, one local 100 nF from VDD to GND. A's SEL pins 1/4/11/15 and
B's SEL pins 1/4 -> SD_IO_EN. B's unused SEL pins 11/15 -> GND; unused
signal pins 9/10/13/14 -> explicit no-connect. SD_IO_EN has a 10k pulldown.
This is the exact RSV pin table, not a renumbered PW symbol. Audit the
footprint and pad-one orientation at the PCB stage; native symbol and
connection checks do not qualify assembled placement geometry.
[TI TMUX1511 datasheet, pin functions and electrical tables](https://www.ti.com/lit/ds/symlink/tmux1511.pdf).

Five 47k pullups connect CMD and DAT0..DAT3 card-side nets to VDD_SD.
No CLK pullup and no pullup to +3V3_MCU on a card-side signal. Six
ESD441DPYR devices have pin 1 IO on the respective card-side net and pin 2
GND at the socket. Use short protection-return paths.

The primary ESD441 datasheet retrieved 2026-09-12 is now SLVSH26C,
superseding SLVSH26B. Section 5.4 gives a recommended IO-to-GND range
of 0..5.5 V; the revision removes the earlier negative-range claim.
Do not treat this unidirectional device as supporting a -5.5 V steady
input. Section 5.6 specifies 5.5 V reverse stand-off with IIO <100 nA
across the operating temperature range. Its separate leakage row is
1 nA typical, 50 nA maximum at VIO=5.5 V and TA=25 C; do not extend that
50 nA maximum to all temperatures. Line capacitance is 1 pF typical at
VIO=0 V, f=1 MHz, 30 mV peak-to-peak and TA=25 C, with no specified
maximum. These limits support selection for positive 3.3 V SD signals,
but do not guarantee assembled bus capacitance, 25/50 MHz timing or
board ESD/EMC performance. Qualify signal integrity and transient residuals
on the installed circuit. [TI ESD441 SLVSH26C, sections 4, 5.4, 5.6 and
revision history](https://www.ti.com/lit/ds/symlink/esd441.pdf).

J2.4 -> VDD_SD; J2.6 and MP1/MP3/MP4/MP5/MP6 -> GND. Preserve native
CMS-012: J2.MP2/R72.2/R73.1/D6.1 are the detect contact node, R72.1 ->
+3V3_MCU, R73.2 -> SD_CD_N -> U1.P406/F14; D6.2 -> GND. R72=10k,
R73=1k; no intentional CD capacitor; debounce a stable state for 20 ms.
Do not connect this mechanical detect circuit to VDD_SD or DAT3.

Native additional GPIO connections:
U1.P106/N6 -> SD_PWR_REQ; U1.P708/N12 -> SD_IO_REQ; U1.P407/R17 <-
SD_READY. P407 is a GPIO status input, NOT SD1CD. The saved host and
hierarchy connections were checked against these assignments. The silicon mapping is
[RA8P1 datasheet Table 1.17](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet).

## SD-001: Exact power and control contracts

| Role / exact MPN | Pin-to-net contract |
| --- | --- |
| U17 / TPS22950CDDCR, DDC SOT-23-6 | 1 ON -> SD_PWR_ON; 2 VIN -> +3V3_MCU; 3 GND -> GND; 4 ILIM -> R76 -> GND; 5 VOUT -> VDD_SD; 6 FLT -> SD_READY |
| U18 / TPS3808G30DBVR, DBV SOT-23-6 | 1 RESET -> SD_READY; 2 GND -> GND; 3 MR -> SD_PWR_ON; 4 CT -> no-connect; 5 SENSE -> VDD_SD; 6 VDD -> +3V3_MCU |
| U19 / SN74LVC1G97DBVR | 1 IN1 and 2 GND -> GND; 3 IN0 -> SD_PWR_REQ; 4 Y -> SD_PWR_ON; 5 VCC -> +3V3_MCU; 6 IN2 -> MCU_RESET_N |
| U20 / SN74LVC1G97DBVR | 1 IN1 and 2 GND -> GND; 3 IN0 -> SD_IO_REQ; 4 Y -> SD_IO_EN; 5 VCC -> +3V3_MCU; 6 IN2 -> SD_READY |

Each gate and supervisor gets its own local 100 nF bypass. SD_PWR_REQ,
SD_IO_REQ and SD_PWR_ON each get a 10k pulldown. SD_READY gets one 10k
pullup to +3V3_MCU; the two open-drain outputs, GPIO input and gate input
share that node. Do not substitute a push-pull supervisor. Gate IN1=0
selects Y=IN0 AND IN2, so power requires both firmware request and released
MCU reset; bus enable requires request and ready. SD_READY low means
disabled, undervoltage or reported switch fault, not one uniquely decoded
fault. [TI SN74LVC1G97 function/pin tables](https://www.ti.com/lit/ds/symlink/sn74lvc1g97.pdf).

Use TPS22950C, not the different TPS22950L latch-off pin/function variant.
The selected switch provides current limiting, reverse blocking and
quick-output discharge. Its FLT output reports thermal shutdown or reverse
current, not current limiting or an output short alone (Table 9-1). The
G30 supervisor must detect VDD_SD undervoltage during an overload; do not
wait for FLT to indicate a short. Neither output is a latched fault record. The 2.21k programming row is 0.38/0.50/0.62 A
min/typ/max under its stated conditions. Neither its typical response time
nor an extrapolated resistor corner is a guaranteed peak-current bound.
[TI TPS22950 electrical table and Equation 4](https://www.ti.com/lit/ds/symlink/tps22950.pdf).

The G30 supervisor senses card voltage, not the upstream rail. Its nominal
threshold is 2.79 V; using +/-1.5% gives 2.74815..2.83185 V falling, and
a conservative 2.902647 V rising screen including maximum hysteresis.
CT open gives 12..28 ms release delay. MR low also holds reset low.
The supervisor is main-powered so a normal SD power-off request can
isolate the bus without waiting for the card capacitor to discharge.
[TI TPS3808 threshold, MR and timing tables](https://www.ti.com/lit/ds/symlink/tps3808.pdf).

The power gate's low-level contract uses the SN74LVC1G97 electrical-table
VOL <=0.1 V at IOL <=100 uA over VCC=1.65..5.5 V, not its less suitable
16 mA row. This leaves 0.25 V below TPS22950C ON VIL=0.35 V while the
parts remain within their operating supplies. TPS3808 MR has a minimum
70k internal pullup: screen its adverse current at 3.6/70k=51.428571 uA.
The 10k external ON pulldown reaches 9.801k with 1% initial tolerance and
100 ppm/C over 100 C. Counting 0.1/9.801k=10.203041 uA as an additional
load is conservative (that ground leg actually assists a low output).
Allocate another 12 uA for combined adverse control leakage and board
leakage; the total is 73.631612 uA, below the 100 uA test condition by
26.368388 uA. Verify the installed leakage allocation; do not add uncounted
fanout. The switch's low-state smart pulldown also assists the low state.
This proves the valid-supply low level, not correct logic during arbitrary
supply collapse, nor the high-level/input-threshold acceptance discussed
below. [TI gate electrical table](https://www.ti.com/lit/ds/symlink/sn74lvc1g97.pdf),
[TI supervisor MR table](https://www.ti.com/lit/ds/symlink/tps3808.pdf),
[TI switch ON limits](https://www.ti.com/lit/ds/symlink/tps22950.pdf).

## SD-002: Passive population and discharge sizing

| Required population | Exact selected MPN / connection |
| --- | --- |
| R76, 2.21k | YAGEO RC0603FR-072K21L; U17.4 to GND |
| R82, 330 ohm | YAGEO RC0603FR-07330RL; VDD_SD to GND, always fitted |
| Six 33 ohm series resistors | YAGEO RC0603FR-0733RL; host-side bus paths above |
| Five 47k pullups | YAGEO RC0603FR-0747KL; card CMD/DAT to VDD_SD |
| Five 10k controls | YAGEO RC0603FR-0710KL; four pulldowns and one SD_READY pullup |
| One 10 uF input | TDK C3216X7R1V106K160AC; U17 VIN to GND |
| One 22 uF output | Samsung CL32B226MOJNNNE; VDD_SD to GND near socket |
| Seven 100 nF | TDK C1608X7R1H104K080AA; switch input/output, two muxes, two gates, supervisor |
| Six bus clamps | TI ESD441DPYR; one per bus signal, as above |

The 330 ohm resistor intentionally costs about 10 mA while the card is
on. It permits discharge without assuming the switch's powered QOD works
at zero VIN and avoids a held-rail discharge controller. Power the island
off when unused. Its exact current manufacturer sheet gives 1%, 100 ppm/C,
0.1 W at 70 C; the calculation retains a conservative 200 ppm/C screen.
The 2.21k part is 1%, 100 ppm/C. The 100 C resistor excursion below is a
conservative calculation interval, not an extension of socket/card ambient
ratings. [YAGEO 330 ohm](https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-07330RL),
[YAGEO 2.21k](https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-072K21L).

The capacitors use existing project donor MPNs; retain their actual
MPNs and verify bias/temperature/aging curves against
[PWR-006](main_regulator_tps63806.md). The 8.976 uF effective-output screen
below assumes 60% bias retention, -20% tolerance and -15% temperature; it
is not a manufacturer-guaranteed combined minimum. The whole card island
must measure <=40.4865 uF for the discharge calculation, including the
inserted card. That ceiling budgets 10 uF inside the card and positive
tolerance/temperature for the installed 22.1 uF, rather than claiming all
cards have this capacitance. Added direct main capacitance is nominal
10.6 uF; added switched card capacitance is nominal 22.1 uF plus the card.
The local native inventory agrees with these nominal sums; complete
source-charged inventory and effective-capacitance acceptance remain open.

## SD-003: Main-rail budget and reproducible screens

| Simultaneous load allocation | A |
| --- | ---: |
| RA8P1 | 0.750 |
| RADIO-019 radio module and support | 0.505 |
| SDRAM | 0.250 |
| NOR | 0.250 |
| Existing logic/control | 0.050 |
| microSD card | 0.250 |
| Added SD support, including bleeder | 0.020 |
| Revised main total, no established overlap | 2.075 |

RADIO-019 allocates 500 mA to the module plus 5 mA to radio support.
The old 50 mA existing-logic line has no itemized proof of overlap here.
Use 2.075 A until that mapping is established. Only if its 50 mA already
includes the radio 5 mA may the table's non-radio logic become 45 mA and
the total remain 2.070 A. This is accounting, not permission to remove
loads or concurrency. See [RADIO-019](radio_interface.md).

These inherited MCU/memory/radio allocations are not simultaneous maximum
datasheet measurements. Camera sensor rails, audio amplifier and display
power still need their own allocations in the complete system budget.
The native TPS63806 offers a plausible prototype path, but this record
does not upgrade its current, inductor, source-path or thermal acceptance.
At 3.2 V input and assumed 75% efficiency, this rail alone needs 2.934 A
input and dissipates 2.347 W at the 2.075 A/upper-voltage screen. The
proven-overlap 2.070 A case gives 2.926 A input and 2.341 W loss. Battery support
and charge reduction remain necessary under limited USB input; do not
claim USB-only full-load operation. See [system power](system_power_design.md).

Use a separate, potentially shared peripheral 3.3 V converter only if
measured total loading/thermal margin or added camera/audio loads require
it. Moving SD to that converter does not remove its demand from the
battery/source budget or eliminate powered-off bus isolation. Qualifying
the revised existing rail is the first prototype option.

This standalone Python block verifies arithmetic and internal allocations,
not hardware. The 0.216 ohm card-power path screen uses 0.116 ohm switch
(the conservative 1.8 V table row) plus 0.100 ohm PCB/socket allowance;
validate installed resistance over operating conditions. It uses the full
270 mA allocation for voltage drop, including support rather than just the
250 mA card. The scaled ILIM corners are a planning model around the
specified 2.21k row, not new TI guaranteed limits. The discharge result is
conditional on source cutoff and isolation within 10 ms, <=20 uA total
return current, and the stated capacitance ceiling. TMUX's zero-supply
leakage test alone does not prove that return-current bound during ramps.

```python
from math import isclose, log

vmain_lo, vmain_hi = 3.151819680019, 3.393012496197
base = .750 + .505 + .250 + .250 + .050
card, support = .250, .020
total = base + card + support
assert isclose(total, 2.075)
total_if_overlap = total - .005
assert isclose(total_if_overlap, 2.070)
vcard_lo = vmain_lo - (card + support) * (.116 + .100)
hs_current = .72 / vcard_lo
assert vcard_lo > 2.79 * 1.015 * 1.025 > 2.7
assert hs_current < card
pout = vmain_hi * total
pin = pout / .75
iin, loss = pin / 3.2, pin - pout
assert 2.933 < iin < 2.934 and 2.346 < loss < 2.347
assert isclose(vmain_hi * total_if_overlap / .75 / 3.2, 2.926473277969912)
assert isclose(vmain_hi * total_if_overlap * (1/.75 - 1), 2.34117862237593)

ron_pd_min = 10000 * .99 * .99
mr_pull_current = 3.6 / 70000
pd_low_screen = .1 / ron_pd_min
control_leakage = 12e-6  # Combined installed acceptance allocation.
gate_low_load = mr_pull_current + pd_low_screen + control_leakage
assert isclose(gate_low_load, 73.631611935807e-6)
assert gate_low_load < 100e-6
assert .1 < .35  # Gate VOL versus switch ON VIL, within valid supplies.

rilim_lo, rilim_hi = 2210 * .99 * .99, 2210 * 1.01 * 1.01
ilim_lo = .38 * (2210 / rilim_hi) ** 1.072
ilim_hi = .62 * (2210 / rilim_lo) ** 1.072
assert .370 < ilim_lo < .375 and .630 < ilim_hi < .635
assert card + support < ilim_lo
fault_main_screen = base + support + ilim_hi  # Conservative double count.

rbleed_lo, rbleed_hi = 330 * .99 * .98, 330 * 1.01 * 1.02
bleed_current = vmain_hi / rbleed_lo
bleed_power = 3.6 ** 2 / rbleed_lo
assert bleed_current < support and bleed_power < .05
pull_current = 5 * vmain_hi / (47000 * .99 * .99)
assert bleed_current + pull_current + .005 < support
cout_effective = 22e-6 * .8 * .85 * .6
cisland_max = (22 * 1.2 * 1.15 + .1 * 1.1 * 1.15 + 10) * 1e-6
assert isclose(cout_effective, 8.976e-6)
assert isclose(cisland_max, 40.4865e-6)
ireturn = 20e-6  # Installed-system acceptance allocation, not proven here.
vfinal = ireturn * rbleed_hi
assert vfinal < .3
tdischarge = .010 + rbleed_hi * cisland_max * log(
    (3.6 - vfinal) / (.3 - vfinal))
assert tdischarge < .045
direct_uf = 10 + .1 + 2 * .1 + 2 * .1 + .1
switched_uf = 22 + .1
assert isclose(direct_uf, 10.6) and isclose(switched_uf, 22.1)

for name, value in (
    ('main total A', total), ('main if proven overlap A', total_if_overlap),
    ('power gate low load A', gate_low_load), ('card low V', vcard_lo),
    ('0.72 W card current A', hs_current), ('source current A', iin),
    ('converter loss W', loss), ('ILIM model min A', ilim_lo),
    ('ILIM model max A', ilim_hi), ('fault main screen A', fault_main_screen),
    ('bleeder max normal A', bleed_current), ('bleeder screen W', bleed_power),
    ('pullups all-low A', pull_current), ('discharge screen s', tdischarge),
):
    print(name, f'{value:.9f}')
print('Arithmetic PASS; physical acceptance remains open')
```

## Sequencing and prototype acceptance

1. Reset/default: external pulldowns keep requests low; MCU_RESET_N blocks
   SD power. Native U19.6 adds one gate input to MCU_RESET_N, without a
   supervisor MR pullup or AON_HOLD load. Its 5 uA input allocation was
   not reserved in the former 12 uA MCU/U7/flash screen: use 17 uA device
   loading and 30 uA total including the retained 13 uA remainder for
   supervisor, board and probe leakage. The total gives 0.376190439 mA
   reset sink and 2.845789680 V released high at minimum normal rail;
   U19 reduces the modeled high margin by 51.005 mV. These are conditional
   DC allocations; TI input-current test points and typical capacitance
   do not qualify all-state loading, receiver thresholds or edge timing.
   See [RST-002 reset fanout arithmetic](reset_coordination_tps3890.md).
2. Insert/power: debounce CD, keep IO request low, assert power request,
   wait for SD_READY with a bounded timeout, configure host idle levels,
   enable IO, then send the SD-required initialization clocks/commands.
3. Normal off: stop new filesystem work, flush, wait for card busy release
   with a bounded timeout, stop CLK, deassert IO request, wait >=1 us with
   valid main power, then deassert power request. Wait >=50 ms before
   another power-on, conditional on the measured discharge screen above.
4. Removal/fault/reset: terminate DMA/transactions, mark the medium lost,
   deassert both requests and require a fresh initialization before reuse.
   Auto-retry is not a firmware fault latch. A brief FLT pulse can release
   while voltage remains valid, so hardware alone does not guarantee that
   stale IO_REQ can never reconnect after a transient fault.

Bench acceptance is finite: first qualify 25 MHz, then 50 MHz with selected
cards, long read/write/CRC tests and actual simultaneous music/camera/radio
workloads. Scope both ends of CLK/CMD/DAT; tune the initial 33 ohm values
against driver impedance, trace topology and load. Check setup/hold and
overshoot against RA8P1 SDHI1_B 3.3 V timing, not generic GPIO timing.
For channel 1_B receivers use VIH=0.625*VCC and VIL=0.25*VCC; include
switch resistance, card drive and pullup/leakage in the assembled DC test.
The mux's 4.5 ohm and 6 pF maximum table entries inform this test but do
not prove whole-board SI; its 3 GHz bandwidth is not an SD clock rating.

Verify startup, hot insertion/removal, card short, brownout, MCU reset,
main power removal and repeated power cycles across the intended battery,
temperature and card set. Measure main droop, current-limit peaks, card
voltage, logic thresholds, isolation latency, residual card voltage and
back-power. Record effective capacitance, source-path current, regulator/
inductor/switch temperatures and ESD residuals. Check the gate thresholds
over the actual main-rail range; do not promote interpolation between
TI voltage-specific rows to guaranteed limits. Include power-off and
partial-ramp leakage, not just powered steady-state tests.

There is no universal power-fault guarantee: supervisory propagation is
finite, some response data is typical, main-powered controls lose normal
specifications during collapse, and unexpected removal can corrupt a
write. TMUX powered-off protection covers its specified signal/supply
conditions, not every possible system ramp. This circuit is a reasonable
prototype implementation with explicit acceptance, not arbitrary-fault
certification. No extra held control rail or hard fault latch is proposed.

## Interface coexistence and firmware boundary

SDHI1_B does not consume the reserved SDRAM, OSPI0 or SSI1_A audio pins
(P907/P906/P206). The selected MIPI camera control reservations
P501/P709/P511/P512/P010 and dedicated PHY can coexist. The old parallel
DVP camera overlaps P400/P405/P406 and cannot coexist with this native SD
mapping; use the selected MIPI camera path. P700 is radio-owned, but this
socket needs no SD1WP. Optional 8-bit eMMC has an audio conflict and is
not a substitute that may be silently added. Preserve the existing
camera/display MIPI resource constraints in CMS-002.

Firmware issue [#845](https://github.com/bsikar/ra8-firmware/issues/845)
tracks the incorrect connector enum (swapped CLK/CMD, wrong CD/WP and
instance). Use SDHI instance 1 and the table above. The validated SCI0
SPI microSD example on P601/P603/P602/P604 is useful protocol/filesystem
reference, not proof of this SDHI pinmux, power sequence or DMA path.
No firmware changes or hardware validation are claimed by this record.

## Sourcing decision

Observed 2026-09-12, USD, stock unreserved, excluding tax/shipping;
independently refreshed by the IC reviewer. All four exact DigiKey parts
were listed Active. Exact electrical and package authority remains the
linked manufacturer datasheet; this table is a purchasing snapshot, not
native population or bench acceptance.

| Selected part / supplier identity | Stock | Lead time | Unit USD at 1 / 10 / 100 |
| --- | ---: | --- | --- |
| TMUX1511RSVR / DigiKey 296-53443-1-ND | 110,392 | 9 weeks | 0.68 / 0.485 / 0.3805 |
| TPS22950CDDCR / DigiKey 296-TPS22950CDDCRCT-ND | 7,698 | 9 weeks | 0.87 / 0.620 / 0.4898 |
| TPS3808G30DBVR / DigiKey 296-17194-1-ND | 11,592 | 16 weeks | 1.84 / 1.355 / 1.0984 |
| SN74LVC1G97DBVR / DigiKey 296-15581-1-ND | 26,850 | 9 weeks | 0.23 / 0.160 / 0.1213 |

Sources: [DigiKey TMUX1511RSVR](https://www.digikey.com/en/products/detail/texas-instruments/TMUX1511RSVR/9954161),
[DigiKey TPS22950CDDCR](https://www.digikey.com/en/products/detail/texas-instruments/TPS22950CDDCR/18187757),
[DigiKey TPS3808G30DBVR](https://www.digikey.com/en/products/detail/texas-instruments/TPS3808G30DBVR/666724),
[DigiKey SN74LVC1G97DBVR](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC1G97DBVR/571196).
TI lists [TMUX1511RSVR as active](https://www.ti.com/product/TMUX1511/part-details/TMUX1511RSVR).
The earlier PWR option's limited-stock observation is historical. RSV
preserves the electrical function; the selected RSV package is now used
in the native schematic.

The same 2026-09-12 refresh verified
[Mouser 595-TMUX1511RSVR](https://www.mouser.com/ProductDetail/Texas-Instruments/TMUX1511RSVR?qs=PqoDHHvF648fktrCSdjlhA%3D%3D):
15,274 stock, 9-week lead time, USD 0.68 / 0.485 / 0.381 at 1 / 10 / 100.
This supersedes the earlier stale indexed count and failed retrieval.
Mouser retrievals for the other three selected ICs failed; their current
Mouser stock and prices remain unverified. Native interconnections and
targeted XML checks and completed native BOM reconciliation are recorded above.

Donor passive/clamp purchasing snapshot refreshed 2026-09-12 from the
linked exact DigiKey pages, USD cut tape, unreserved stock. All nine were
listed Active. These are selected procurement identities; native parts
are placed and their local capacitor population matches the contract.
Exported BOM identity and quantity reconciliation passed against the native XML.

| Exact MPN / DigiKey cut-tape identity and source | Stock | USD at 1 / 10 / 100 |
| --- | ---: | --- |
| RC0603FR-072K21L / [311-2.21KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-072K21L/727018) | 208,654 | 0.10 / 0.025 / 0.0122 |
| RC0603FR-07330RL / [311-330HRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-07330RL/730109) | 384,039 | 0.10 / 0.025 / 0.0122 |
| RC0603FR-0733RL / [13-RC0603FR-0733RLCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0733RL/727158) | 1,055,511 | 0.10 / 0.025 / 0.0122 |
| RC0603FR-0747KL / [311-47.0KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0747KL/727253) | 1,985,054 | 0.10 / 0.025 / 0.0122 |
| RC0603FR-0710KL / [311-10.0KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/726880) | 2,356,440 | 0.10 / 0.025 / 0.0122 |
| C3216X7R1V106K160AC / [445-14799-1-ND](https://www.digikey.com/en/products/detail/tdk/C3216X7R1V106K160AC/3956465) | 50 | 0.68 / 0.424 / 0.2903 |
| CL32B226MOJNNNE / [1276-3395-1-ND](https://www.digikey.com/en/products/detail/samsung-electro-mechanics/CL32B226MOJNNNE/3891481) | 162,494 | 0.52 / 0.318 / 0.2122 |
| C1608X7R1H104K080AA / [445-1314-1-ND](https://www.digikey.com/en/products/detail/tdk/C1608X7R1H104K080AA/513811) | 329,450 | 0.11 / 0.060 / 0.0359 |
| ESD441DPYR / [296-ESD441DPYRCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/ESD441DPYR/28715599) | 2,456 | 0.34 / 0.207 / 0.1291 |

The 10 uF input donor has only 50 immediately available; its 100-piece
price tier is not a claim of 100-piece stock. Keep the exact donor for
this prototype implementation and recheck quantity availability before purchase.
Do not silently substitute capacitance/package or use stale project donor
stock figures as today's inventory. Capacitor curve and installed leakage
acceptance remain separate from purchasing availability.
