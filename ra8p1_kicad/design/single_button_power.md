# Single exposed power button: engineering basis

Revision 7, 2026-09-07. Tracking: [inputs #832](https://github.com/bsikar/ra8-firmware/issues/832),
[power #825](https://github.com/bsikar/ra8-firmware/issues/825), and
[radio #826](https://github.com/bsikar/ra8-firmware/issues/826).
Cross-references: [system power](system_power_design.md),
[service access](service_interface.md), [boot and debug](boot_and_debug.md),
and [five-control allocation CMS-009](camera_storage_interfaces.md#cms-009-five-exposed-controls-and-wake-allocation).

This is a calculation record with partial native implementation: the input,
ESD, PDT timing, INT and KILL networks are drawn as mapped in BTN-002 below.
The two MCU signals are verified end-to-end through the hierarchy. It is not a
complete or qualified power circuit. The converter-enable source qualification, actual
battery operating envelope and rail discharge remain release gates below.
BTN-009 closes the paper MCU pin/DC allocation; BTN-002 records the native
signal-route checkpoint, not hardware qualification. Do not mark the
remaining gates complete solely from this document.

## BTN-001: Product behavior and topology

Use one exposed normally-open momentary **power/wake/recovery** switch.
There are five exposed controls in total: this power key, previous page,
next page, volume down and volume up. CMS-009 specifies the four direct
MCU keys; they do not duplicate or bypass this hardware power latch.
Boot straps and engineering reset remain internal paired service pads,
not extra enclosure buttons.
Choose LTC2954ITS8-1#TRPBF for the button-controller candidate. It provides
hardware latching and an interrupt that follows a debounced press; firmware
can implement tap-to-sleep/wake and a longer orderly-shutdown gesture.

```text
Protected pack / USB charger -> SYS_AON -> buck-boost -> +3V3_MCU -> loads
                                      -> hold-up isolation -> AON_HOLD
                                                             -> controller

SW1 -> PB               INT -> POWER_BUTTON_N -> MCU wake input
       LTC2954-1       KILL <- POWER_KILL_N    <- MCU open drain
                        EN -> SYS_EN_REQ -> source-valid logic -> MAIN_PWR_EN
```

Charging does not pass through the switched application rail. USB attachment
must not override a user's hard-off request. The charger and necessary
source monitoring remain powered when the processor is off. A charger
QON/reset input is not a second exposed button or a substitute for the
independent application-power latch.

| Starting condition | Button action | Required behavior |
| --- | --- | --- |
| Fresh source connection | None | Application stays off; charging follows pack-qualified policy |
| Off, valid source | Press then release | Start application; no firmware needed to latch power |
| Running or retained sleep | Tap | Debounced interrupt; firmware sleeps or wakes |
| Running | Hold about 2 s | Firmware may offer shutdown, save state, sequence display off, then assert KILL |
| Frozen firmware | Keep holding | Hardware eventually removes application power regardless of firmware |
| Forced off | Release, then press again | Restart only after source-valid and rail-discharge conditions are satisfied |
| Battery too low | Any | Source qualification prevents uncontrolled restart or brownout cycling |

The 2 s behavior is a proposed UI policy, not a resistor-programmed hardware
threshold. Hard-off is a recovery action and can interrupt a storage write
or an e-paper update. Filesystem recovery, brownout handling and display
re-initialization are required; a hard-off button cannot promise atomic
storage or graceful high-voltage shutdown after the processor freezes.

The [LTC2954 datasheet, Rev. B, pp. 8-12](https://www.analog.com/media/en/technical-documentation/data-sheets/2954fb.pdf)
defines initial-off behavior, interrupt tracking, KILL blanking, and the
turn-off lockout. The original turn-on press must be released; do not promise
that holding the initial power-on press continuously also executes recovery.

## BTN-002: Native schematic connection basis

Use the TS8 pinout for the exact ITS8 ordering code, not the DFN pinout.
Functional passive identifiers below map to assigned references where
implemented; the cross-map following the table identifies those references.
Additional unplaced SYS-007 parts do not yet have native references here.

| Pin | Name | Connection |
| --- | --- | --- |
| 1 | VIN | Candidate AON_HOLD, with C_BYP = 100 nF to GND locally; held-up control supply must be qualified by SYS-007/BTN-010 |
| 2 | PB | R_PB = 10 kOhm to raw SYS_AON; R_SW = 1 kOhm to POWER_KEY_EXT/SW1; SW1 other contact to GND; C_PB = 100 nF from PB to GND; ESD441DPYR from POWER_KEY_EXT to GND |
| 3 | ONT | Intentional no-connect; use internal debounce |
| 4 | GND | Common ground |
| 5 | INT | POWER_BUTTON_N; R_INT = 10 kOhm to switched +3V3_MCU; RA8P1 P303/B6, IRQ29-DS; BTN-009 |
| 6 | EN | SYS_EN_REQ/MAIN_PWR_EN; active-high open drain; final pullup/domain and source-valid clamp belong to SYS-007; BTN-008's 100k is a standalone compatibility example, not the final clamp circuit |
| 7 | PDT | C_PDT = 1 uF to GND; nominal hard-off delay calculated in BTN-004 |
| 8 | KILL | POWER_KILL_N; R_KILL = 10 kOhm to switched +3V3_MCU; R_KILL_PD = 100 kOhm to GND; RA8P1 P903/D9 NMOS open-drain shutdown output; separate source-fault NMOS drain, never directly tied to EN |

### Native reference and calculation cross-map

Read-only snapshot of [power_button.kicad_sch](../ereader/power_button.kicad_sch)
on 2026-09-07 for this eight-sheet controls checkpoint. All eleven components below
have `in_bom=yes`, `dnp=no`, and exact manufacturer-part fields. This is
saved-file connectivity/metadata verification, not a measured hardware test.

| Native reference | Functional role / value | Exact native Manufacturer_Part_Number | Calculation record |
| --- | --- | --- | --- |
| U9 | LTC2954-1, TS8 button controller | LTC2954ITS8-1#TRPBF | BTN-001/002/004/009/010 |
| C55 | C_BYP, 100 nF local U9 VIN bypass | C1608X7R1H104K080AA | BTN-002/006; PWR-001 bypass basis |
| C56 | C_PB, 100 nF PB filter | C1608X7R1H104K080AA | BTN-003/005/010 |
| R18 | R_PB, 10 kOhm raw-SYS pullup | RC0603FR-0710KL | BTN-003/005: 9801..10201 ohm |
| R19 | R_SW, 1 kOhm switch series resistor | RC0603FR-071KL | BTN-003/005: 980.1..1020.1 ohm |
| SW1 | Normally-open POWER key | EVQP7A01P | BTN-003/005/010: 10 uA at 2 V minimum, 0.5 ohm initial contact maximum |
| C57 | C_PDT, 1 uF forced-off timing | C2012X7R1E105K125AB | BTN-004/005: 6475.256410 ms nominal; capacitance-only screen 0.765..1.265 uF |
| D1 | Exposed power-key protection, IO1/GND2 | ESD441DPYR | BTN-003/010 |
| R20 | R_INT, 10 kOhm switched-MCU pullup | RC0603FR-0710KL | BTN-009: INT input and sink-current margins |
| R21 | R_KILL, 10 kOhm switched-MCU pullup | RC0603FR-0710KL | BTN-003/005/009: KILL bias and MCU open-drain sink |
| R22 | R_KILL_PD, 100 kOhm pulldown | RC0603FR-07100KL | BTN-003/005/009: off-state bias |

The displayed U9 value `LTC2954ITS8-1` and SW1 value `POWER` are readable
functional labels, not ordering-code mismatches; their hidden exact MPNs
are present. C55/C56 match the selected 100 nF, 50 V, X7R, +/-10% part;
C57 matches the selected 1 uF, 25 V, X7R, +/-10% part. No value or MPN
mismatch was found against BTN-003/004/005/006.

Confirmed native connection sets:

```text
SYS_AON: U9.1, C55.1, R18.1                  [temporary raw VIN supply]
PB:      U9.2, R18.2, R19.2, C56.1
SW node: R19.1, SW1.2, D1.1
PDT:     U9.7, C57.1
INT:     U9.5, R20.2, U1.B6/P303             [POWER_BUTTON_N]
KILL:    U9.8, R21.2, R22.1, U1.D9/P903      [POWER_KILL_N]
MCU rail: R20.1, R21.1 on +3V3_MCU
GND:     U9.4, C55.2, C56.2, C57.2, SW1.1, D1.2, R22.2
ONT:     U9.3 has one intentional no-connect marker
EN:      U9.6 remains unwired; it is not marked intentionally NC
```

The input/filter/switch, protection, timing and INT/KILL pull/bias networks
are now wired. Native XML verifies both MCU routes and the actual R19 pin
numbers above; no calculation depends on swapping that symmetric resistor's
terminal numbers. SYS-007 source qualification and discharge are not
implemented by this checkpoint. The important remaining supply
split is **U9.1 and C55.1 to AON_HOLD, while R18.1 stays on raw SYS_AON**.
The existing shared wire is the earlier raw-supply stage, not implementation
of SYS-007. Do not rename the whole shared net to AON_HOLD.

The 2026-09-07 CLI full-project ERC reports 204 errors and two active warnings.
The native GUI review also displays 15 excluded warnings (17 warnings
including exclusions, 221 total displayed findings); exclusions are not fixes.
Page 7
has exactly the unconnected U9.6/EN and undriven SYS_AON findings; neither
INT/KILL signal nor its assigned MCU pin has an ERC finding. Page 8's four
page/volume circuits are fully routed as recorded in CMS-009 and have no
ERC findings. This is not approval to power or fabricate the unfinished
board. D1..D5 remain included in simulation by the owner's choice; no
validated model or simulation-qualified circuit is claimed.
The final native page-7 draft note explicitly identifies the unfinished
SYS_AON source, EN route, held supply, supervision and discharge, with links
to SYS-007 and this record. The final C55 text-clearance adjustment changes
presentation, not connectivity. The same-checkpoint source path above is
the reference; a movable-text-only save does not invalidate the electrical
connection table.

The sheet's `BTN-003: POWER-KEY INPUT / R18, R19, C56, SW1` note
(native UUID `bd9d6b7a-5ac8-470a-8918-e857e43f19e2`) points back to
BTN-003/005/010. Its `BTN-004: HARDWARE FORCED-OFF TIMING / C57` note
(UUID `df4fcea1-8a05-4c8f-a3b3-016ac8ea0cbf`) points to BTN-004/005.
Both use the valid relative path `../design/single_button_power.md`.
These reference/identifier pairs are the bidirectional math trace: a part
or native note can be followed to the derivation, and this table back to
the corresponding native parts. Live UI object IDs are not durable links.

The native BTN-003 note's strict bounds were corrected and read back from
saved file `acb25cd72627`: source <435.13 uA, sink >587.88 uA, contact
current >255.34 uA, open contact >=2.87748 V and discharge peak <4.694 mA.
They conservatively enclose the BTN-005 values 435.121620 uA,
587.889477 uA, 255.341556 uA, 2.87748599 V and 4.693399 mA. The earlier
bound-rounding annotation issue is resolved; component selections are unchanged.

Pin mapping is from the [manufacturer pin configuration and pin functions, pp. 2 and 5-6](https://www.analog.com/media/en/technical-documentation/data-sheets/2954fb.pdf).
There is no conductive SYS_AON or AON_HOLD pullup on either MCU signal. Open-drain
connections must not be replaced with push-pull outputs during library
cleanup. A named no-connect on ONT is correct; it is not a DNP component.

R_KILL brings KILL high as the switched rail rises, allowing blank firmware
and programming tools to keep power without a boot-time software handshake.
The pulldown defines its off-state. Firmware initializes its shutdown pin
as high impedance/open drain, never push-pull high, and only pulls low after
orderly shutdown. This is not a watchdog: a crashed MCU remains powered
until the user forces off or another independent protection acts.

BTN-009 and CMS-009 reserve the actual BGA289 pins and distinguish retained
sleep from reset-on-wake modes. Full-off power-on is performed by the
controller, not an unpowered MCU. The AON_HOLD supply is a coordinated
source-loss design candidate, not an established charger output or a
completed hold-up calculation in this document.

## BTN-003: Switch loading and logic checks

The Panasonic [EVQP7A01P specification](https://industrial.panasonic.com/cdbs/www-data/pdf/ATK0000/ATK0000C378.pdf)
has a minimum rated load of 10 uA at 2 V, 0.5 Ohm maximum initial contact
resistance, and a 50 mA at 12 V maximum resistive load. Its 10 ms bounce is
shorter than the controller's 26 ms minimum internal debounce. Switch
operating temperature is -20..70 C; the enclosure/product range must honor
that limitation even if the IC grade is wider.

An ultra-low-current internal PB pullup alone is not a sound basis for
reusing this switch. R_PB supplies adequate contact current during a press
without a continuous closed path when the button is released. The 1 kOhm
series resistor also limits discharge of C_PB into the contacts.

For the provisional SYS_AON screen of 3.0..4.6 V, use resistor production
tolerance of 1% and an additional 100 ppm/C over 100 C from nominal:
R_min = R_nom * 0.99 * 0.99; R_max = R_nom * 1.01 * 1.01. This is a
calculation allowance, not a qualified battery limit or lifetime drift model.

At PB = 0.6 V, the worst external source current plus the datasheet's
15 uA internal source limit and 12 uA adverse external leakage allocation
are less than the current through R_SW and the
closed contact. The PB node therefore cannot remain above that minimum
falling threshold in the DC model. Evaluating KCL at the specified threshold
avoids extrapolating the internal pullup current to an unspecified voltage.

For the minimum contact-current/open-contact-voltage screen, allocate an
additional 12 uA of unwanted sink leakage. BTN-010 assigns 0.1 uA of this
to ESD441DPYR; 11.9 uA remains for the other external leakage paths.
The full 12 uA is conservatively used in either adverse direction in the
threshold tests. It is an allocation, not a new component specification.

Do not copy the datasheet's optional 5.1 kOhm series-noise resistor unchanged
when also adding the 10 kOhm external pullup: that divider would keep PB too
high. The proposed 1 kOhm/100 nF values are a separate circuit calculation.
The controller's component-level ESD rating is not enclosure-level IEC ESD
qualification; BTN-010 qualifies the selected TVS's DC compatibility only.

KILL's maximum rising threshold is 0.68 V using the specified threshold
and hysteresis maxima. The 10 kOhm/100 kOhm bias exceeds that threshold
comfortably on the provisional 3.0 V minimum application rail even with a
12 uA leakage allocation. BTN-009 checks P903's guaranteed 0.5 V maximum
low against the 0.57 V KILL minimum falling threshold and P303's input
against INT's 0.4 V maximum low. Electrical limits are in the
[LTC2954 table, pp. 3-4](https://www.analog.com/media/en/technical-documentation/data-sheets/2954fb.pdf).

## BTN-004: Delay and restart calculations

ONT is open: turn-on debounce is 32 ms typical, 26..41 ms specified. For PDT:

```text
C_PDT [uF] = 1.56e-4 * (t_added [ms] - 1)
t_force_off,nom = 64 ms + 1 ms + 1 uF / (1.56e-4 uF/ms)
                = 6475.256410 ms, approximately 6.48 s
```

The [manufacturer timing equation, pp. 9 and 11](https://www.analog.com/media/en/technical-documentation/data-sheets/2954fb.pdf)
supports this nominal calculation. It is not a guaranteed 6.48 s timer.
The electrical table's added-delay bounds are tested at 1500 pF; this record
does not extrapolate them as a guaranteed bound for 1 uF.

Candidate C_PDT is TDK C2012X7R1E105K125AB, 1 uF, 25 V, X7R, +/-10%,
0805. It is listed as production by [TDK](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C2012X7R1E105K125AB).
Initial tolerance and X7R temperature alone screen 0.765..1.265 uF, giving
4.969..8.174 s through the nominal formula. This interval explicitly excludes
timer IC variation, DC-bias dependence, aging, and board leakage. The
[TDK characterization sheet](https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/c2012x7r1e105k125ab.pdf)
is a typical-curve reference, not guaranteed end-of-life capacitance.
Obtain an acceptable bounded recovery-time specification and confirm it
with the IC/capacitor data and qualification measurements before release.

KILL has a 400 ms minimum turn-on blanking period. The actual switched rail
must raise KILL through 0.68 V before blanking expires; measure this with
the full inrush load and slowest valid source. Firmware speed is not needed
for that bias network, but regulator startup time still matters.

The controller specifies a minimum 200 ms enable restart lockout. That
does not establish a cold reboot: TPS63802 load disconnect is not a
guaranteed active output-discharge resistor. With only a 66 kOhm feedback
divider and an illustrative 100 uF rail, decay from 3.3 V to 0.3 V takes
15.826 s. A hypothetical switched 100 Ohm bleeder gives 23.979 ms for the
same isolated capacitance. Neither capacitance nor bleeder is a final
whole-board design. Inventory all effective rail capacitance, back-power
paths, device power-cycle thresholds and discharge-switch delays before
selecting the real network. Re-arm must wait for the slowest relevant rail.

## BTN-005: Python-verifiable arithmetic

The following standard-library block was executed with assertions. It
verifies arithmetic under named assumptions; it does not qualify hardware.

```python
from math import isclose, log

f_min, f_max = 0.99 * 0.99, 1.01 * 1.01
rpu_min, rpu_max = 10000 * f_min, 10000 * f_max
rs_min, rs_max = 1000 * f_min, 1000 * f_max
contact_max = 0.5
vsrc_min, vsrc_max = 3.0, 4.6  # Provisional screen, not implemented UVLO.
leak_alloc = 12e-6

# DC threshold crossing proof, using specified PB current at 0.6 V.
pb_source_at_threshold = (vsrc_max - 0.6) / rpu_min + 15e-6 + leak_alloc
pb_sink_at_threshold = 0.6 / (rs_max + contact_max)
assert pb_sink_at_threshold > pb_source_at_threshold
contact_min = vsrc_min / (rpu_max + rs_max + contact_max) - leak_alloc
switch_open_min = vsrc_min - leak_alloc * rpu_max
switch_ext_open_min = switch_open_min - 0.1e-6 * rs_max
contact_peak = vsrc_max / rs_min  # Charged filter-capacitor discharge.
assert contact_min > 10e-6 and switch_ext_open_min > 2.0
assert contact_peak < 0.05
pullup_power_max = vsrc_max**2 / rpu_min
assert pullup_power_max < 0.1

# KILL high screen with an intentionally pessimistic leakage allocation.
rpd_min = 100000 * f_min
kill_high_min = (vsrc_min / rpu_max - leak_alloc) / (1 / rpu_max + 1 / rpd_min)
kill_sink_max = 3.6 / rpu_min + leak_alloc
assert kill_high_min > 0.68
assert kill_sink_max < 1e-3

t_force_nom_ms = 64 + 1 + 1 / 1.56e-4
c_temp_low, c_temp_high = 1 * 0.90 * 0.85, 1 * 1.10 * 1.15
t_cap_only_low_ms = 65 + c_temp_low / 1.56e-4
t_cap_only_high_ms = 65 + c_temp_high / 1.56e-4
assert isclose(t_force_nom_ms, 6475.256410256411)

rail_c_example = 100e-6
t_divider_s = 66000 * rail_c_example * log(3.3 / 0.3)
t_bleeder_s = 100 * rail_c_example * log(3.3 / 0.3)
assert t_divider_s > 0.2 and t_bleeder_s < 0.2

for name, value in (
    ("PB source at threshold, uA", pb_source_at_threshold * 1e6),
    ("PB sink at threshold, uA", pb_sink_at_threshold * 1e6),
    ("contact minimum, uA", contact_min * 1e6),
    ("open contact minimum, V", switch_open_min),
    ("external switch open minimum incl. TVS series drop, V", switch_ext_open_min),
    ("contact transient peak, mA", contact_peak * 1e3),
    ("pullup power upper screen, mW", pullup_power_max * 1e3),
    ("KILL high minimum screen, V", kill_high_min),
    ("KILL sink upper screen, mA", kill_sink_max * 1e3),
    ("forced-off nominal, ms", t_force_nom_ms),
    ("cap-only low/high nominal-formula, ms", (t_cap_only_low_ms, t_cap_only_high_ms)),
    ("66k divider discharge example, s", t_divider_s),
    ("100 Ohm bleeder discharge example, s", t_bleeder_s),
):
    print(name, value)
```

## BTN-006: Sourcing candidates

Public distributor pages retrieved 2026-09-07; USD, excluding shipping and
tax. Quantities are snapshots, not a reservation or guaranteed availability.
Functional quantities are for this candidate subcircuit only; annotate actual
references and populate the project BOM when implementing the native sheet.

| Role / quantity | Exact manufacturer part | Distributor part | Stock | Unit at 1 / 10 / 100 |
| --- | --- | --- | ---: | --- |
| Controller / 1 | LTC2954ITS8-1#TRPBF | [DigiKey LTC2954ITS8-1#TRPBFCT-ND](https://www.digikey.com/en/products/detail/analog-devices-inc/LTC2954ITS8-1-TRPBF/1621452) | 5,128 | $7.83 / $6.056 / $5.1224 |
| Same controller alternative source | LTC2954ITS8-1#TRPBF | [Mouser 584-C2954ITS8-1TRPBF](https://www.mouser.com/ProductDetail/Analog-Devices/LTC2954ITS8-1TRPBF?qs=hVkxg5c3xu%2FjT4Y9VauCgQ%3D%3D) | 2,157 | $7.14 / $5.49 / $4.62 |
| Power switch / 1; four additional keys in CMS-009 | EVQP7A01P | [DigiKey P16763CT-ND](https://www.digikey.com/en/products/detail/panasonic-industry/EVQ-P7A01P/4429447) | 32,462 | $0.28 / $0.248 / $0.2076 |
| PDT capacitor / 1 | C2012X7R1E105K125AB | [DigiKey 445-1354-1-ND](https://www.digikey.com/en/products/detail/tdk/C2012X7R1E105K125AB/513887) | 33,852 | $0.23 / $0.13 / $0.0814 |
| R_PB, R_INT, R_KILL / 3 | RC0603FR-0710KL | [DigiKey 311-10.0KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/729827) | 2,904,275 | $0.10 / $0.025 / $0.0122 |
| R_SW / 1 | RC0603FR-071KL | [DigiKey 311-1.00KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-071KL/726843) | 4,068,534 | $0.10 / $0.025 / $0.0122 |
| R_KILL_PD / 1; final R_EN belongs to SYS-007 | RC0603FR-07100KL | [DigiKey 311-100KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-07100KL/729836) | 2,200,236 | $0.10 / $0.025 / $0.0122 |
| Power-key TVS / 1; four additional in CMS-009 | ESD441DPYR | [DigiKey 296-ESD441DPYRCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/ESD441DPYR/28715599) | 3,273 | $0.34 / $0.207 / $0.1291 |
| Same TVS alternative source | ESD441DPYR | [Mouser 595-ESD441DPYR](https://www.mouser.com/ProductDetail/Texas-Instruments/ESD441DPYR?qs=bpu3f%2FCR1jziUA14lbLOFw%3D%3D) | 11,380 | $0.34 / $0.149 / $0.129 |

C_BYP and C_PB reuse TDK C1608X7R1H104K080AA from the existing
[PWR-001 bypass selection](power_decoupling.md); refresh its supplier snapshot
at BOM entry. Resistor tolerance/TCR basis is the manufacturer's
[10 kOhm RC0603 specification](https://yageogroup.com/component-documentation/download/specsheet/RC0603FR-0710KL)
and [1 kOhm specification](https://yageogroup.com/component-documentation/download/specsheet/RC0603FR-071KL).

LTC2954 costs more than newer button ICs but provides explicit initial-off
behavior and a held-button interrupt without additional state decoding.
MAX16150AUT+T is not a drop-in cost reduction: its [datasheet, p. 12](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX16150.pdf)
requires initialization of the output state after supply application, and
its interrupt behavior differs. TPS3424A11C13ADRLR is another candidate,
but its [datasheet](https://www.ti.com/lit/ds/symlink/tps3424.pdf) requires
rechecking the switched-domain interrupt interface when SYS_AON can be
below 3.3 V. Neither alternative is approved by this comparison.

## BTN-007: Release gates, ERC and schematic annotation

1. Close SYS_AON bounds against the actual protected battery and charger,
   and the independent AON_HOLD energy/current proof in SYS-007/BTN-010.
   LTC2954's guaranteed operating minimum is 2.7 V. BQ25619E battery
   depletion cutoff is below this; it does not prove safe button operation
   to empty. Implement pack-appropriate undervoltage lockout and hysteresis.
   Nominal 3.0 V in a calculation is not a protection circuit.
2. Complete SYS_EN_REQ to MAIN_PWR_EN source qualification. BTN-008 verifies
   direct logic compatibility using TPS63802's EN-specific thresholds;
   no extra Schmitt buffer is needed for that purpose. Qualify the actual
   undervoltage/source-valid clamp, its leakage, low-voltage startup state,
   hysteresis and temperature. Include service access without permitting
   firmware or a powered debugger to defeat the hard-off path.
3. Size independent rail discharge and prohibit back-power through USB,
   debugger, radio, display, audio, storage and sensor pins. Test with USB
   attached, battery absent, debugger attached, frozen firmware and rapid
   release/repress. Charging can remain active; application rails must not.
4. Preserve the verified BTN-009/CMS-009 native pin routes and implement
   the firmware wake contract;
   confirm KILL rise within 400 ms, and bound hard-off timing. Include the
   button controller, source logic and charger in measured sleep current.
   The controller's typical 6 uA headline is not a whole-product off budget.
5. Follow the service document for MD/external reset and radio boot/reset
   contacts. Confirm a blank or broken host still permits radio recovery.
   A power button does not replace MCU ROM boot-entry sequencing.
6. In KiCad, use supply symbols for actual rails, power flags only on real
   source nets as required by ERC, open-drain pin types, and local pullups.
   Run ERC after each connected section; no blanket exclusions for this
   circuit. Unimplemented release gates must remain visible, not disguised
   as no-connects or approved power outputs.
   The earlier six-pin passive-type gap is resolved: generic defaults are
   Bidirectional, with P303_IN/P309_IN/P310_IN/P311_IN/P909_IN selected as
   Input and P903_OD selected as Open collector. Native export confirms
   those effective types. Preserve the assignments during library updates;
   this narrow correction does not validate unrelated GPIO pin types or
   prove firmware configuration, contention immunity or powered-off behavior.
   P903 must still be configured NMOS open drain in firmware; never
   substitute push-pull high. POWER_BUTTON_N is input on the MCU leaf and
   output on the button leaf; POWER_KILL_N is output on the MCU leaf and
   input on the button leaf, with matching root sheet pins and explicit
   wires. The four page/volume nets are MCU-leaf inputs. None is a power
   net or requires a PWR_FLAG; silicon pin names/numbers are unchanged.

Suggested concise native-sheet note, linked to this calculation identifier:

```text
BTN-001..010: design/single_button_power.md; direct keys: CMS-009.
Five keys total; one power/wake/recovery key. BOOT/RESET: internal pads.
INT=P303/B6; KILL=P903/D9 open drain; both pulls to switched +3V3_MCU.
PDT 1 uF: 64 + 1 + 1/(1.56e-4) = 6475 ms nominal hard-off.
10k PB pullup / 1k series: contact current >=255 uA (3.0 V screen).
Battery UVLO, source-valid clamp, rail discharge and timing qualification OPEN.
```

Remove the final OPEN line only after the corresponding calculations,
native connections and qualification evidence are added and reviewed.

## BTN-008: Corrected direct regulator-enable interface

The earlier generic VIH/VIL comparison omitted the converter's precise EN
thresholds. [TPS63802 SLVSEU9D, section 8.5, p. 6 and section 9.3.2](https://www.ti.com/lit/ds/symlink/tps63802.pdf)
specifies EN rising at 1.07..1.13 V and falling at 0.97..1.03 V. Thus
LTC2954's maximum 0.4 V EN low leaves **0.57 V** to the minimum falling
threshold. Its low-output guarantee is specified at 500 uA; this section's
standalone pullup example requires less than one tenth of that. This is not a zero-margin
interface, and an added SN74LVC1G17 buffer is not justified by that claim.

The historical R_EN = 100 kOhm to SYS_AON example below proves standalone
DC compatibility, not the selected source-loss circuit. SYS-007 supersedes
its pullup and supply-domain assumptions when adding a held control supply,
supervisor, inverter and separate discharge/KILL transistors. Recalculate
all final loads there. Either latch or source protection must force EN low;
never parallel a push-pull high output. Do not implement the old SYS-005
EN-only clamp: it does not independently establish fault-latched shutdown.

For the existing 3.0..4.6 V source screen and resistor tolerance/TCR budget,
R_EN is 98.010..102.010 kOhm. Use 1 uA for controller off leakage, 0.2 uA
for TPS63802 input leakage, and allocate 1 uA to the not-yet-selected clamp
and board leakage. The controller table tests 0.1 uA at 1 V and 1 uA at
26.4 V; using the larger number for this lower-voltage screen is an
engineering allowance, not an additional ADI test specification. Confirm
the final leakage allocation over the actual operating domain.

```text
EN_high_min = 3.0 - (1 + 0.2 + 1) uA * 102.010 kOhm = 2.775578 V
Rising-threshold margin = 2.775578 - 1.13 = 1.645578 V
Falling-threshold margin = 0.97 - 0.4 = 0.57 V
Worst sink-current screen = 4.6/98.010k + 1.2 uA = 48.133986 uA
EN pullup current while off <= 4.6/98.010k = 46.933986 uA
```

The 0.2 uA input-leakage limit is the converter's `Ilkg` specification;
its 100 nA `IFB` limit applies to the feedback pin and must not be used for
EN. The current through R_EN while off belongs in the actual off-current
budget; the controller's 6 uA headline omits this resistor loss.

```python
from math import isclose

r_en_min = 100000 * 0.99 * 0.99
r_en_max = 100000 * 1.01 * 1.01
en_high_min = 3.0 - (1e-6 + 0.2e-6 + 1e-6) * r_en_max
en_rise_margin = en_high_min - 1.13
en_fall_margin = 0.97 - 0.4
en_sink_max = 4.6 / r_en_min + 1.2e-6
assert isclose(en_high_min, 2.775578)
assert en_rise_margin > 1.64 and en_fall_margin > 0.56
assert en_sink_max < 500e-6
print("EN high minimum / rising / falling margins, V",
      en_high_min, en_rise_margin, en_fall_margin)
print("EN sink / off pullup current, uA",
      en_sink_max * 1e6, 4.6 / r_en_min * 1e6)
```

This block was executed successfully. Native drawing can use the verified
TS8 mapping, PB network and MCU reservations of BTN-002/009; it must keep
the SYS-007 source-valid interface visibly incomplete until closed. This
DC margin proof does not guarantee behavior when LTC VIN drops below
2.7 V, or establish battery UVLO, rail discharge, or restart qualification.

## BTN-009: Actual RA8P1 interrupt and shutdown pins

The selected package is R7KA8P1KFLCAC#UC0, MIPI-enabled BGA289. Reserve:

| Net | Port / ball | Configuration | Power domain |
| --- | --- | --- | --- |
| POWER_BUTTON_N | P303 / B6 | Input, IRQ29-DS, no internal pull; CMS-009 wake contract | VCC = +3V3_MCU |
| POWER_KILL_N | P903 / D9 | GPIO NMOS open drain; released during normal operation | VCC = +3V3_MCU |

Authority: [RA8P1 datasheet R01DS0439EJ0130](../../docs/reference/ra8p1-datasheet.pdf),
Tables 1.17, 2.5 and 2.7; and
[Hardware Manual R01UH1064EJ0130](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
Tables 21.2/21.16, pp.849/876-877, and PmnPFS description, pp.853-855.
P903 exists on the MIPI289 variant and supports NCODR NMOS-open-drain mode.
Its optional IRQ1, GPT and LCD functions are not enabled. The CMS-006
reservation test now includes P903 separately from the five key inputs;
it has no collision with reserved SDRAM, OSPI0, SDHI1, MIPI camera control,
radio, SSI1_A or optional four-bit SDHI0 storage. This is not permission
to reuse the still-unallocated display/audio-control GPIOs without auditing.

For INT, use the Schmitt thresholds at the actual VCC: high >=0.8*VCC
and low <=0.2*VCC. P303's 5V-tolerant input has a 5 uA leakage limit;
its tolerance does not justify moving the pullup to an always-on rail.
Allocate another 2 uA to released INT and 1 uA to board leakage. The
2 uA allocation exceeds ADI's published 1 uA test at VINT=3 V; it is an
engineering envelope to confirm at the screened 3.6 V maximum.

With +3V3_MCU=3.0..3.6 V and R_INT=9801..10201 ohm, the worst screened
high is 2.918392 V, leaving 0.518392 V to the 2.4 V high threshold.
INT's 0.4 V maximum low at 3 mA leaves 0.200000 V low margin at 3 V.
The conservative sink load is 0.373310 mA, below that output test current.
Keep P303 input-only, with ISEL enabled and its internal pull disabled.
Sleep-mode wake routing and reset-on-deep-standby behavior are in CMS-009;
this allocation does not assert that an unpowered MCU can wake itself.

For P903, the generic output guarantee is **VOL <=0.5 V at IOL=1 mA**,
not the 0.4 V row for I2C/ESWM functions. The existing 10k/100k KILL
bias needs at most 0.379310 mA with 12 uA of adverse leakage. Therefore
the guaranteed DC low margin is only 0.57-0.50 = **0.070 V**. It is
positive, but small: use a short local connection and common ground;
include any final source-fault transistor leakage in the 12 uA allocation.
A topology change or greater required noise margin needs a fresh check,
not a typical GPIO VOL curve promoted to a guarantee. The same bias
screens KILL high at >=2.606319 V, well above its 0.68 V rising bound.

Initialization contract, with the normal PFS write protection procedure:
keep PDR=0 while selecting GPIO (PMR=0), ISEL=0, PCR=0 and NCODR=1;
load PODR=1 (released), then set PDR=1. Low drive is sufficient for this
current. Do not briefly enable the reset-default PODR=0 as an output.
For orderly shutdown, save/stop all consumers and set PODR=0; keep it
asserted until power is lost, rather than issuing a marginal short pulse.
No firmware init is required to hold power while the pin remains high-Z.

Both pullups stay on +3V3_MCU when VIN moves to AON_HOLD. P903 is not a
5V-tolerant pin; a live AON_HOLD pullup would create an off-state injection
risk. The independent fault path may add a separate NMOS drain at KILL,
with source at GND and gate driven by the held-domain OFF_H signal.
That is not an electrical short between KILL and EN. Check its off-state
leakage and on-state low in SYS-007 and account for added gate load there.

If corrupt firmware actively holds KILL low, the fixture must hold
J1.10/MCU_RESET_N low **before** application power-on, keeping GPIOs in
reset/high-Z. Apply the appropriate MD/debug boot request, wait for valid
power and the required reset hold, then release RES per SERVICE-001.
TP1/MR is not the direct reset contact. J1.1 is voltage sense, not a power
injection point. This recovery path remains subject to lifecycle/debug
authorization and must not bypass source qualification or hard-off.

```python
from math import isclose

r_min, r_max = 10000*.99*.99, 10000*1.01*1.01
v_min, v_max = 3.0, 3.6
int_high = v_min - (5+2+1)*1e-6*r_max
int_high_margin = int_high - .8*v_min
int_low_margin = .2*v_min - .4
int_sink = v_max/r_min + (5+1)*1e-6
kill_sink = v_max/r_min + 12e-6
kill_high = (v_min/r_max - 12e-6)/(1/r_max + 1/(100000*.99*.99))
kill_low_margin = .57 - .5
assert isclose(int_high, 2.918392)
assert int_high_margin > .518 and int_low_margin > .199
assert int_sink < 3e-3 and kill_sink < 1e-3
assert kill_high > .68 and isclose(kill_low_margin, .07)
print("INT high / high margin / low margin V", int_high, int_high_margin, int_low_margin)
print("INT / KILL max sink mA", int_sink*1e3, kill_sink*1e3)
print("KILL high / low margin V", kill_high, kill_low_margin)
```

## BTN-010: Power-key ESD and held-control-domain boundary

Use ESD441DPYR pin 1 on POWER_KEY_EXT, pin 2 to GND. Place the 1k
between the external switch/TVS node and PB; retain 100 nF at PB and the
10k pullup to **raw SYS_AON**, not AON_HOLD. There is no protection-diode
connection to either supply rail. Quantity is one here plus four in
CMS-009: five purchased TVSs total for the five exposed controls.

```text
SYS_AON -- 10k -- PB -- 1k -- POWER_KEY_EXT -- SW1 -- GND
                 |              |
               100nF         ESD441DPYR.1
                 |           ESD441DPYR.2
                GND             |
                               GND
LTC2954 VIN -> AON_HOLD (SYS-007 candidate, separate from raw SYS_AON)
```

[TI ESD441 SLVSH26B, sections 4 and 5.6](https://www.ti.com/lit/ds/symlink/esd441.pdf)
supports the DPY pin mapping, 5.5 V positive stand-off and <100 nA leakage
across operating temperature in that range. The 4.6 V raw-source maximum
screen leaves 0.9 V of stand-off headroom. The 25 C leakage row is not the
full-temperature limit. Assign 0.1 uA of the existing 12 uA external
budget to this diode; do not add it again outside that budget.

BTN-005 was re-executed with all 12 uA adversarially sourcing current in
the PB-low check: at PB=0.6 V, sink is 587.889 uA versus 435.122 uA
source, leaving approximately 152.768 uA. Contact current is >255.34 uA;
the open external contact is >=2.87748 V including the TVS drop across the 1k.
The 100 nF contact-discharge peak screen is 4.693399 mA, below 50 mA.
These checks retain the selected switch's 10 uA/2 V minimum load without
assuming its internal controller pullup has a suitable minimum current.

The DPY clamp datum at 6 A is 8.6 V **typical**, not a maximum and not
a 5.5 V clamp. This selection is a DC/function and sourcing qualification,
not proof of the waveform at PB during enclosure ESD. Short return layout,
resistor pulse stress and residual positive/negative PB voltage require
physical verification later; no whole-product IEC pass is claimed.

The coordinated SYS-007 direction is a held supply for LTC VIN and control
logic; a raw-SYS supervisor clamps MAIN_PWR_EN. A held-domain inverter
generates OFF_H from EN, driving separate rail-discharge and KILL NMOS
devices. Final component values and hold-up duration belong to SYS-007.
This replaces the EN-only SYS-005 direction, not the MCU-domain pullups.
For an early source fault, the supervisor's **minimum** recovery delay
must exceed the LTC's 650 ms maximum KILL blank plus recognition/propagation
allowance. A nominal 1.7 s target alone does not prove that inequality.
An early qualification-period press may be aborted if KILL blanking ends
before the supervisor releases EN. A late intentional press can survive
until release and produce a short delayed start; the product accepts this
SYS-007 behavior. The circuit does not enforce a fresh release-and-repress
sequence after qualification. A source fault without a new press must
still clear the previous on latch. Test both ends of the qualification
interval and repeated source faults; do not confuse accepted delayed
execution of a user's press with unattended restart of the previous state.

Keeping R_PB on raw SYS avoids routing its held-key current through the
hold-up capacitor. It does **not** remove the LTC internal PB pullup load:
with SYS=0 or a held key, PB can approach ground and drain the held domain.
The [ADI pin description and application diagrams, pp.6/14](https://www.analog.com/media/en/technical-documentation/data-sheets/2954fb.pdf)
depict nominal internal bias/resistance, but do not specify a maximum PB
current at PB=0. Do not extrapolate the 15 uA limit tested at PB=0.6 V
as a guaranteed held-key limit. SYS-007 needs a bounded current allocation
and validation covering this path, timing-cap current and all other loads.

PB's above-VIN absolute tolerance also does not establish zero injection
with VIN below its 2.7 V operating minimum. Test/obtain a bound for raw
source reconnect and slow AON_HOLD decay, including key held/released;
do not infer off-state PB-to-VIN isolation from an absolute maximum.
While those supply-loss checks remain open, retain the 3.0..4.6 V raw
and 3.0..3.6 V MCU arithmetic as conditional operating screens, not a
claim that every transition keeps the controller in its specified range.

## BTN-011: Recovery-route audit and remaining native edits

Read-only saved-file review on 2026-09-07, against SERVICE-001..006.
The reviewed clocks/debug and radio sheets retain commit `3f9ca4c3f6`'s
service-pad wiring; their SHA256 prefixes were `962cc69477b6` and
`769d5e013a07`. The IO sheet prefix was `a767244220a7`. KiCad editing
continues in parallel; recheck any changed file before treating this list
as its current state. Pin endpoints, wires and local labels were traced
from the native files; this audit did not generate or alter any schematic.

### Existing connections to preserve

| Saved connection | Finding |
| --- | --- |
| J1.1 -> +3V3_MCU; J1.3/.5/.9 -> GND | Correct target-reference contacts; not fixture power input |
| J1.2 -> U1.C6/P210; J1.4 -> U1.D6/P211 | SWDIO and SWCLK already routed |
| J1.6 -> U1.E7/P209; J1.8 -> U1.C7/P208 | Already suitable TXD9 and RXD9 physical routes; R5 pulls RXD9 up locally |
| J1.10 -> U1.D5/RES, U2.1/RESET and R1.2 | Correct direct MCU reset access; root also routes reset to U7.3 |
| TP1.1 -> U2.3/MR; TP2.1 -> U1.E6/P201/MD and R2.2 | Correct separate internal reset-request and boot contacts; both pad 2 contacts are GND |
| J1.7 | Deliberate NC/key, not a DNP assembly choice |
| TP3.1 -> U3.15/GPIO9 and R16.2; TP3.2 -> GND | Correct internal C6 boot contact; R15 pulls GPIO8 high on the switched radio rail |
| U7.4 -> U6.3; U8.4 -> U5.8 and R9.1 | Original normal-mode reset and SPI-enable paths, not service arbitration |
| RADIO_PWR_EN -> R7.1 and U8.6 | Original host-dependent power path; R7.2/R8.1 drive U4.3 |
| U3.24/RXD0 and U3.25/TXD0 | Unwired, with no service translator/contact group present |
| U1.D9/P903 and U1.B6/P303 | Now connected through the root to KILL and INT respectively; current BTN-002 snapshot supersedes the original unwired reservation |

The RA8 recovery paths do not need a second UART header. However,
[HUM Table 60.42, p.3635](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware)
assigns **MD to pin 4 in the Renesas emulator's SCI personality**. This
board keeps J1.4 on SWCLK and exposes MD separately at TP2. Its fixture
must use a deliberate J1-plus-TP2 adapter; do not claim a standard Renesas
10-pin SCI cable is directly compatible or short J1.4 to TP2. A concise
native service note should identify J1.6=target TXD9, J1.8=target RXD9,
MD=TP2, and the prohibition on simultaneous driving debug/SCI fixtures.

### Minimal native completion checklist

1. Finish the shared application-power prerequisites in BTN-002/009 and
   SYS-007: source-valid latch clearing and discharge. The switched-rail
   KILL bias, P903 open-drain route and P303 interrupt route are now wired;
   preserve their verified hierarchy and pin functions. Do not replace
   those routes with no-connects. No service override may force EN past the
   source clamp or feed application rails through J1/SERVICE_VIO.
2. Place the four SERVICE-003 SN74LVC1G97DBVR mux roles on the radio sheet,
   or an explicitly connected child sheet. Each uses pin 5=+3V3_MCU,
   2=GND, 6=SERVICE_VIO and a local 100 nF bypass. Preserve U7/U8 and
   their existing input wiring. Assign native references through annotation;
   the role names below are not preassigned U-numbers.
3. Make these three **cuts and insertions**, not parallel output additions:

   | Role | Pin 1 normal input | Pin 3 service input | Pin 4 output / native edit |
   | --- | --- | --- | --- |
   | SERVICE_POWER | RADIO_PWR_EN | +3V3_MCU | RADIO_PWR_REQ_EFF -> R7.1; remove raw host from R7.1 only, retaining U8.6 on raw RADIO_PWR_EN |
   | SERVICE_RESET | U7.4, renamed RADIO_MR_NORMAL_N | SERVICE_C6_RESET_N | RADIO_MR_N -> U6.3; remove the direct U7.4-to-U6.3 connection |
   | SERVICE_SPI | U8.4, renamed SPI_IO_EN_NORMAL | GND | SPI_IO_EN -> U5.8/R9.1; remove the direct U8.4-to-U5.8/R9.1 connection |
   | SERVICE_UART | GND | C6_EN | SERVICE_UART_OE -> TXU0202.6, with its own 47k pulldown |

4. Add TXU0202DCUR and the five SERVICE-002 internal contacts. Exact
   translator connection: pin 1=U3.25/TXD0; 2=GND; 3=SERVICE_VIO;
   4=SERVICE_RX; 5=SERVICE_TX; 6=SERVICE_UART_OE; 7=+3V3_RADIO;
   8=U3.24/RXD0. Put one 100 nF on each supply. Add local 10k input
   pullups at pins 5/1 to SERVICE_VIO/+3V3_RADIO respectively, and
   47k output pullups at pins 4/8 to the corresponding same-domain rails.
   Contacts remain 1=GND, 2=SERVICE_VIO, 3=SERVICE_TX, 4=SERVICE_RX,
   5=SERVICE_C6_RESET_N. TP3 remains the separate boot contact.
5. Add two parallel 10k SERVICE_VIO pulldowns and a 10k pullup from
   SERVICE_C6_RESET_N to +3V3_MCU. Retain U6, its divider/delay and C6_EN
   radio-rail pullup: service must request reset through MR, not drive
   C6_EN directly. Change the RADIO-015/016/018 native notes to identify
   the inserted muxes and SERVICE-003/006; do not leave U7/U8 described
   as the final direct drivers after rerouting.
6. Populate exact MPN/supplier fields, then run ERC and a fresh saved-net
   review. Check that raw and effective net names are distinct; only one
   push-pull output drives each effective control; SERVICE_VIO has no
   conductive supply tie to +3V3_MCU/+3V3_RADIO/AON_HOLD; both translator
   directions and all pullup domains match step 4. Export the full PDF/BOM
   through the existing export workflow after the native change.

Incremental component count for that service proposal: four muxes, one
TXU0202, six 100 nF bypasses, five 10k resistors, three 47k resistors and
one five-contact internal service group. This excludes the existing J1,
TP1..3, U7/U8, power-button section and later fixture-protection additions.
No extra enclosure switch is required. Unwired U3.4/U3.26 handshake/ready
routes remain a separate RADIO-008/009 completion task; UART ROM recovery
does not require connecting them directly across power domains.

### Verified facts and remaining electrical acceptance

The mux pin map and selector polarity were checked independently against
[TI SCES416N Table 1](https://www.ti.com/lit/ds/symlink/sn74lvc1g97.pdf):
pin 6 low selects pin 1; high selects pin 3. The translator mapping was
checked against [TI SCES942A Table 6-1](https://www.ti.com/lit/ds/symlink/txu0202.pdf).
The following test uses the manufacturer's eight-entry truth table rather
than merely reusing the proposal's mux implementation:

```python
from itertools import product

# Keys are the actual (pin6 IN2, pin1 IN1, pin3 IN0) levels.
truth = dict(zip(product((0, 1), repeat=3), (0, 0, 1, 1, 0, 1, 0, 1)))
for service, power, hw_reset, host_reset, c6_en, fixture_release in product((0, 1), repeat=6):
    normal_reset = truth[host_reset, 0, hw_reset]
    normal_spi = truth[power, 0, c6_en]
    got = (truth[service, power, 1],
           truth[service, normal_reset, fixture_release],
           truth[service, normal_spi, 0],
           truth[service, 0, c6_en])
    want = ((1, fixture_release, 0, c6_en) if service else
            (power, hw_reset & host_reset, power & c6_en, 0))
    assert got == want
print("SERVICE: all 64 complete steady-state combinations pass; not a glitch test")
rlo, rhi = 10000*.99*.99, 10000*1.01*1.01
assert 3.6/(4.7*rlo)+12e-6 < 100e-6  # UART 47k output-pull screen.
assert 3.6/rlo > 100e-6  # Existing R9: NOT the light-load guarantee.
assert 3.6/(2*rlo) > 100e-6  # Existing R7/R8: likewise.
print("UART 47k output sink upper screen, uA", (3.6/(4.7*rlo)+12e-6)*1e6)
```

This truth table resolves blank/corrupt-host *ownership* under valid rails;
it is not full electrical approval. SERVICE_POWER's R7/R8 load and
SERVICE_SPI's R9 exceed 100 uA, so SERVICE-005's rail-wide VCC-0.1/0.1 V
light-load output limits cannot be borrowed for them. The TI mux and
translator Schmitt rows are discrete supply test points; the final
intermediate-rail bounds and ramps still need closure. U6 MR also sources
current when low, without a guaranteed maximum established in the current
record; retain RADIO-015's load gate rather than silently assuming zero.
The new C6_EN mux input and UART circuit add leakage/capacitance to the
radio startup/discharge and EN budgets.

For RA8 SCI, P208/RXD9 is not a 5V-tolerant input. Qualify target-referenced
fixture logic and power-off high-Z behavior separately from the C6 adapter;
a fixed independent 3.3 V adapter is not automatically compatible during
target droop or hard-off. Keep J1.10 reset asserted before powering the
application to prevent corrupt code from asserting P903/KILL. In C6-only
service, hold MCU reset throughout, keeping P903 high-Z while the passive
KILL bias sustains the latch. No short from KILL to a live fixture rail is
needed or permitted. Use the real power key to latch the application after
source qualification; release and retry if pressed during qualification.

RA8 SCI entry requires TP2/MD low and an external RES cycle, not POR alone;
hold RES at least 3 ms after valid VCC. Allow the HUM's applicable up-to
1/2/3 s tool-connection time; follow the boot firmware protocol rather than
inventing a shorter timeout. C6 requires GPIO8 high, GPIO9 low before EN rises and for
at least 3 ms afterwards, with at least 50 us supply-stable/reset-low
intervals. These are documented minimums, not new measured timing claims.
[Renesas HUM 4.3-4.4/60.11](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
[Espressif module Tables 4-2/4-3/4-8](https://www.espressif.com/sites/default/files/documentation/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf).
Hold both reset requests while adding/removing SERVICE_VIO; SERVICE-006
provides the complete handover sequence. Test actual recovery with blank
MRAM, corrupt host code, source fault, battery-absent USB and adapter removal.
Lifecycle/authentication restrictions still apply.

Two missing procurement items can now use exact sourced parts, retrieved
2026-09-07 (public-page snapshots, not reservations):

| Role | Exact MPN / supplier | Displayed stock | USD at 1 / 10 / 100 |
| --- | --- | ---: | --- |
| Three 47k resistors | RC0603FR-0747KL / [DigiKey 311-47.0KHRCT-ND](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0747KL/727253) | 2,035,698 | 0.10 / 0.025 / 0.0122 |
| UART translator | TXU0202DCUR / [Mouser 595-TXU0202DCUR](https://www.mouser.com/en/ProductDetail/Texas-Instruments/TXU0202DCUR?qs=t7xnP681wgVssPLUozppFA%3D%3D) | 9,035 | 1.08 / 0.776 / 0.619 |

The [exact YAGEO 47k specification](https://yageogroup.com/component-documentation/download/specsheet/RC0603FR-0747KL)
confirms 1%, 100 ppm/C and 0.1 W at 70 C, matching the conditional
46.0647..47.9447k calculation. The translator's source showed 16-week
factory lead time; resistor 17 weeks. This closes MPN/sourcing selection,
not the fixture's electrical specification, pad protection or mechanics.
