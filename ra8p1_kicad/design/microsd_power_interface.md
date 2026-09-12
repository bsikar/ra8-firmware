# microSD power and SDHI1_B implementation contract

Revision 1, 2026-09-08. Proposed engineering implementation, NOT native
schematic completion, a populated BOM, or bench qualification. Role names
below are placeholders, not assigned schematic references. This record
extends [CMS-003 and CMS-012](camera_storage_interfaces.md); CMS-012 is the
newer authority for the already-native socket and mechanical card detect.

## Architecture and operating envelope

Use the existing +3V3_MCU regulator with a current-limited switched VDD_SD
island and six bilateral bus isolation channels. No separate SD regulator
is selected. The prior 1.800 A main allocation excluded SD; it is neither
a measured load nor the TPS63806's validated output limit. This proposal
raises the continuous allocation to 2.070 A and requires the source and
thermal checks below. Preserve local music, camera capture and radio use;
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
| SD1CLK_B / P400 / P17 | U_SD_BUS_A 2 / 3 | SD_CLK_CARD / 5 |
| SD1CMD_B / P401 / N17 | U_SD_BUS_A 5 / 6 | SD_CMD_CARD / 3 |
| SD1DAT0_B / P402 / L14 | U_SD_BUS_A 10 / 9 | SD_DAT0_CARD / 7 |
| SD1DAT1_B / P403 / H13 | U_SD_BUS_A 14 / 13 | SD_DAT1_CARD / 8 |
| SD1DAT2_B / P404 / J13 | U_SD_BUS_B 2 / 3 | SD_DAT2_CARD / 1 |
| SD1DAT3_B / P405 / G12 | U_SD_BUS_B 5 / 6 | SD_DAT3_CARD / 2 |

U_SD_BUS_A and U_SD_BUS_B are **TMUX1511RSVR**, TI RSV 16-pin UQFN,
2.6 x 1.8 mm, not TMUX1511PWR's 14-pin TSSOP footprint. On both parts:
pin 16 VDD -> +3V3_MCU, pin 8 GND -> GND, pins 7 and 12 NC -> explicit
no-connect, one local 100 nF from VDD to GND. A's SEL pins 1/4/11/15 and
B's SEL pins 1/4 -> SD_IO_EN. B's unused SEL pins 11/15 -> GND; unused
signal pins 9/10/13/14 -> explicit no-connect. SD_IO_EN has a 10k pulldown.
This is the exact RSV pin table, not a renumbered PW symbol. Audit the
symbol, footprint and pad-one orientation before placement.
[TI TMUX1511 datasheet, pin functions and electrical tables](https://www.ti.com/lit/ds/symlink/tmux1511.pdf).

Five 47k pullups connect CMD and DAT0..DAT3 card-side nets to VDD_SD.
No CLK pullup and no pullup to +3V3_MCU on a card-side signal. Six
ESD441DPYR devices have pin 1 IO on the respective card-side net and pin 2
GND at the socket. Use short protection-return paths; the device's typical
capacitance is not a maximum assembled-line capacitance guarantee.
[TI ESD441 datasheet](https://www.ti.com/lit/ds/symlink/esd441.pdf).

J2.4 -> VDD_SD; J2.6 and MP1/MP3/MP4/MP5/MP6 -> GND. Preserve native
CMS-012: J2.MP2/R72.2/R73.1/D6.1 are the detect contact node, R72.1 ->
+3V3_MCU, R73.2 -> SD_CD_N -> U1.P406/F14; D6.2 -> GND. R72=10k,
R73=1k; no intentional CD capacitor; debounce a stable state for 20 ms.
Do not connect this mechanical detect circuit to VDD_SD or DAT3.

Proposed additional GPIOs, currently unallocated by the interface contract:
U1.P106/N6 -> SD_PWR_REQ; U1.P708/N12 -> SD_IO_REQ; U1.P407/R17 <-
SD_READY. P407 is a GPIO status input, NOT SD1CD. Recheck reservations
against the native MCU sheet before placement. The silicon mapping is
[RA8P1 datasheet Table 1.17](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet).

## Exact power and control contracts

| Role / exact MPN | Pin-to-net contract |
| --- | --- |
| U_SD_SW / TPS22950CDDCR, DDC SOT-23-6 | 1 ON -> SD_PWR_ON; 2 VIN -> +3V3_MCU; 3 GND -> GND; 4 ILIM -> R_SD_ILIM -> GND; 5 VOUT -> VDD_SD; 6 FLT -> SD_READY |
| U_SD_PG / TPS3808G30DBVR, DBV SOT-23-6 | 1 RESET -> SD_READY; 2 GND -> GND; 3 MR -> SD_PWR_ON; 4 CT -> no-connect; 5 SENSE -> VDD_SD; 6 VDD -> +3V3_MCU |
| U_SD_PWR_GATE / SN74LVC1G97DBVR | 1 IN1 and 2 GND -> GND; 3 IN0 -> SD_PWR_REQ; 4 Y -> SD_PWR_ON; 5 VCC -> +3V3_MCU; 6 IN2 -> MCU_RESET_N |
| U_SD_IO_GATE / SN74LVC1G97DBVR | 1 IN1 and 2 GND -> GND; 3 IN0 -> SD_IO_REQ; 4 Y -> SD_IO_EN; 5 VCC -> +3V3_MCU; 6 IN2 -> SD_READY |

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
quick-output discharge. The 2.21k programming row is 0.38/0.50/0.62 A
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

## Passive population and sizing

| Population | Exact proposed MPN / connection |
| --- | --- |
| R_SD_ILIM, 2.21k | YAGEO RC0603FR-072K21L; U_SD_SW.4 to GND |
| R_SD_BLEED, 330 ohm | YAGEO RC0603FR-07330RL; VDD_SD to GND, always fitted |
| Six 33 ohm series resistors | YAGEO RC0603FR-0733RL; host-side bus paths above |
| Five 47k pullups | YAGEO RC0603FR-0747KL; card CMD/DAT to VDD_SD |
| Five 10k controls | YAGEO RC0603FR-0710KL; four pulldowns and one SD_READY pullup |
| One 10 uF input | TDK C3216X7R1V106K160AC; U_SD_SW VIN to GND |
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

The capacitors are existing project donor candidates; retain their actual
MPNs and verify bias/temperature/aging curves against
[PWR-006](main_regulator_tps63806.md). The 8.976 uF effective-output screen
below assumes 60% bias retention, -20% tolerance and -15% temperature; it
is not a manufacturer-guaranteed combined minimum. The whole card island
must measure <=40.4865 uF for the discharge calculation, including the
inserted card. That ceiling budgets 10 uF inside the card and positive
tolerance/temperature for the installed 22.1 uF, rather than claiming all
cards have this capacitance. Added direct main capacitance is nominal
10.6 uF; added switched card capacitance is nominal 22.1 uF plus the card.
Recompute the complete source-charged inventory after native placement.

## Main-rail budget and reproducible screens

| Simultaneous load allocation | A |
| --- | ---: |
| RA8P1 | 0.750 |
| ESP32 radio | 0.500 |
| SDRAM | 0.250 |
| NOR | 0.250 |
| Existing logic/control | 0.050 |
| microSD card | 0.250 |
| Added SD support, including bleeder | 0.020 |
| Revised main total | 2.070 |

These inherited MCU/memory/radio allocations are not simultaneous maximum
datasheet measurements. Camera sensor rails, audio amplifier and display
power still need their own allocations in the complete system budget.
The native TPS63806 offers a plausible prototype path, but this record
does not upgrade its current, inductor, source-path or thermal acceptance.
At 3.2 V input and assumed 75% efficiency, this rail alone needs 2.926 A
input and dissipates 2.341 W at the upper voltage screen. Battery support
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
base = .750 + .500 + .250 + .250 + .050
card, support = .250, .020
total = base + card + support
assert isclose(total, 2.070)
vcard_lo = vmain_lo - (card + support) * (.116 + .100)
hs_current = .72 / vcard_lo
assert vcard_lo > 2.79 * 1.015 * 1.025 > 2.7
assert hs_current < card
pout = vmain_hi * total
pin = pout / .75
iin, loss = pin / 3.2, pin - pout
assert isclose(iin, 2.926473277969912)
assert isclose(loss, 2.34117862237593)

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
    ('main total A', total), ('card low V', vcard_lo),
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
   SD power. Added reset-net load is one gate input (5 uA maximum), not a
   supervisor MR pullup. Include it in the reset-source fanout audit.
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

Observed 2026-09-08, USD, stock unreserved, excluding tax/shipping. Exact
electrical and package authority is the linked manufacturer datasheet;
distributor data below establishes the purchasing snapshot only.

| Selected part / supplier identity | Observed stock | Unit USD at 1 / 10 / 100 |
| --- | ---: | ---: |
| TMUX1511RSVR / DigiKey 296-53443-1-ND | 110,663 | 0.68 / 0.485 / 0.3805 |
| TPS22950CDDCR / DigiKey 296-TPS22950CDDCRCT-ND | 7,698 | 0.87 / 0.62 / 0.4898 |
| TPS3808G30DBVR / DigiKey 296-17194-1-ND | 11,734 | 1.84 / 1.355 / 1.0984 |

Sources: [DigiKey TMUX1511RSVR](https://www.digikey.com/en/products/detail/texas-instruments/TMUX1511RSVR/9954161),
[DigiKey TPS22950CDDCR](https://www.digikey.com/en/products/detail/texas-instruments/TPS22950CDDCR/18187757),
[DigiKey TPS3808G30DBVR](https://www.digikey.com/en/products/detail/texas-instruments/TPS3808G30DBVR/666724).
TI lists [TMUX1511RSVR as active](https://www.ti.com/product/TMUX1511/part-details/TMUX1511RSVR).
The earlier PWR option had only 10 at DigiKey and 280 on the retrieved
Mouser page. RSV resolves the stock issue while preserving the electrical
function, with an explicit package change before any native placement.
[Mouser also lists the exact RSV part](https://www.mouser.com/ProductDetail/Texas-Instruments/TMUX1511RSVR?qs=PqoDHHvF648fktrCSdjlhA%3D%3D),
but its indexed 37,399 count was two months old and live retrieval failed;
do not describe that as freshly verified inventory. Refresh all reused
gate/passive/clamp donor stock when creating native procurement fields.
