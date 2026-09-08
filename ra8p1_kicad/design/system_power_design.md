# E-reader system-power engineering basis

Revision 5, 2026-09-07. Tracking: [power #825](https://github.com/bsikar/ra8-firmware/issues/825),
[architecture #823](https://github.com/bsikar/ra8-firmware/issues/823), and
[inputs #832](https://github.com/bsikar/ra8-firmware/issues/832).
This record defines candidate circuit interfaces and verified arithmetic.
It does not claim that these circuits are already placed in KiCad or that
the whole-board power budget is closed. SYS-006 now selects an electrical
battery candidate; purchased-lot, harness and mechanical approval remain
distinct from that selection. SYS-007 supersedes the SYS-005 EN-only clamp
with a concrete held-up control, latch-clear and discharge circuit.
SYS-008 supersedes the earlier charger comparison and SYS-006's tentative
4.16 V / 103AT-2 selections with an autonomous BQ25616 charging circuit.
SYS-009 defines USB-current permission; these sections remain subject to
their explicit qualification gates, not an assertion of fabrication release.
Existing implemented rail consumers are in [the root schematic](../ereader/ereader_rev1.kicad_sch).
Keep the main-regulator voltage calculation authoritative in
[PWR-002](power_decoupling.md#pwr-002-main-rail-regulation-and-reset-headroom).

The later [audio requirement and AUD-005 calculations](audio_subsystem.md#aud-005-audio-rails-thermal-load-and-power-path-impact)
add high-current headphone rails and built-in speakers. The MCU/radio-only
load screen below is not the new product's peak budget and does not approve
the charger or battery for full-power audio plus charging.

## Native held-supply and supervisor implementation checkpoint: 2026-09-07

The saved [power-button sheet](../ereader/power_button.kicad_sch) now contains
U10 LM66100DCKR, R31 100 ohm/1 W, C62 100 nF input bypass, and C63-C66
100 uF/10 V polarized reservoir capacitors. It also contains U11
TPS3808G01DBVR, R32/R33 precision source divider, R34 EN pullup, C67
100 nF bypass, C68 1 nF SENSE filter and C69-C71 100 nF C0G CT bank.
Its visible SYS-007 comments link back to the full calculations below.
This supersedes the raw-supply
connection and missing-reservoir statements in the historical checkpoint.

Read-only native XML netlist assertions verify the corrected connection
basis below; the BOM, PDF and native ERC refresh are also complete.

| Net | Exact members in this control block |
| --- | --- |
| SYS_AON | R18.1, U10.1 VIN, C62.1, R32.1 |
| U10 output | U10.6 VOUT, R31.1 |
| AON_HOLD | R31.2, U10.3 CE, C63.1, C64.1, C65.1, C66.1, U9.1 VIN, C55.1, C67.1, R34.1, U11.3 MR, U11.6 VDD |
| MAIN_PWR_EN (sheet-local) | U9.6 EN, U11.1 open-drain RESET, R34.2 |
| U11 SENSE | U11.5, R32.2, R33.1, C68.1 |
| U11 CT | U11.4, C69.1, C70.1, C71.1 |
| GND | U10.2, U10.5 ST, U11.2, R33.2, C62-C71 pin 2, in addition to existing grounds |

U10.4 is the stock symbol's hidden, electrically `no_connect` NC pin;
it remains unwired, consistent with TI's internally unconnected pin.
There is no raw-to-held wire bridge. A PWR_FLAG on AON_HOLD declares the
real source path from U10 VOUT through passive R31 for ERC; its visible
note identifies that purpose. No flag masks the missing raw SYS_AON source.
C63-C66 positive terminals are on AON_HOLD, negative on GND.
The sixteen held-supply/supervisor fitted components carry matching exact manufacturer/order
numbers and source fields in the native schematic and regenerated BOM.
Footprints remain unqualified and outside this checkpoint's scope.
The regenerated native BOM contains 47 grouped rows, 131 unique included
references and quantity sum 131; C63-C66 form one group of four and C69-C71
form one group of three. The fresh
eight-page A3 PDF was rendered and its corrected power-button page visually
inspected. The other seven page renders are byte-identical to their earlier
visual review. These checks and arithmetic checks do not measure hardware.

The complete power circuit is still unfinished: charger/protected-pack input,
USB permission, main converter, inverter and KILL/discharge FETs remain
absent. CLI ERC reports 203 errors and two warnings, one fewer error than
committed baseline `4b228dacd4`. The removed error is U9 EN unconnected;
the power-button sheet now has only raw SYS_AON power undriven. Native ERC
reports 220 entries including the 15 existing exclusions. No severity or
exclusion settings were changed. The missing source is explicit,
not waived; the remaining errors must be resolved as the power tree is built.
This is an editable progress checkpoint, not a functional power-control or
fabrication release. The corrected 46.589403 ms hold interval is an allocation-based
calculation, not a measured runtime guarantee.

## Historical button-interface checkpoint: 2026-09-07

The following records the earlier button-only state before the held-supply
implementation above. Present-tense implementation statements in this
historical record describe that earlier snapshot, not the current schematic.

This is a progress checkpoint, not a completed power circuit or permission
to fabricate, populate, or energize the full board. Read-only KiCad 10.0.5
netlist and configured ERC checks compared the saved worktree with commit
`3f9ca4c3f68927453ed9b4a8f2b7843043ae6e12`. The implementation snapshot is
[power_button.kicad_sch](../ereader/power_button.kicad_sch) in the same
progress commit as this document, including its explicit incomplete-power
note and final C55 reference/value placement.
Subsequent native edits require a fresh check; this record is not a waiver
of later errors.

The [power-button sheet](../ereader/power_button.kicad_sch) implements U9's
PB input filter, switch and ESD clamp, local bypass, nominal forced-off
timing capacitor, and the following MCU interfaces:

| Verified native net | Members / domain |
| --- | --- |
| POWER_BUTTON_N | U9.5 INT, R20.2, U1.B6 P303 input; R20.1 pulls only to switched +3V3_MCU |
| POWER_KILL_N | U9.8 KILL, R21.2, R22.1, U1.D9 P903 open drain; R21.1 to switched +3V3_MCU and R22.2 to GND |
| Power-key contact node | SW1.2, R19.1, D1.1; D1.2 and SW1.1 to GND |
| PB filter node | U9.2 PB, R18.2, R19.2, C56.1; C56.2 to GND |
| SYS_AON | U9.1 VIN, C55.1, R18.1; temporary raw-source connection, not the SYS-007 held supply |
| PDT / ONT | U9.7 to C57.1, C57.2 to GND; U9.3 intentionally NC for internal ON debounce |

R22's exact MPN is RC0603FR-07100KL, 100 kOhm; its copied 10 kOhm
datasheet link was corrected natively before this final check. D1's
ESD441DPYR IO1/GND2 connection is correct; the generic drawing glyph is
not a validated simulation model. The final ESD and timing qualification
requirements in [BTN-003/004/010](single_button_power.md) still apply.

The source path is **not implemented**: no charger, protected-pack harness,
USB current-permission circuit, TPS63802 main converter, AON_HOLD reservoir,
source supervisor, EN pullup/clamp, or controlled rail-discharge circuit is
placed. U9.6 EN is unconnected. Consequently neither the button's hardware
latch nor the MCU KILL signal currently controls an application supply.
Do not describe the current sheet as a working soft-power circuit or a
safe battery/USB interface. A later SYS-007 implementation must separate
U9.1/C55.1 onto AON_HOLD while keeping R18.1 on raw SYS_AON; renaming their
present common wire would be wrong. The source-loss, battery-rebound,
startup and discharge qualification gates in SYS-007 are not fulfilled by
the completed input/INT/KILL wiring.

The [four page/volume keys](../ereader/user_controls.kicad_sch) are connected
to P309/A12 (previous), P310/E10 (next), P311/B12 (volume down), and P909/B14
(volume up); that sheet has no ERC findings in this snapshot. Together with
SW1, these are five exposed user controls, not five power/boot switches.
Zero local findings do not imply an ERC-clean or finished whole board.

Configured ERC was run with `--severity-all`; no severity, exclusion or
ignore setting was changed to obtain the following delta:

| Checkpoint | Errors | Warnings | Total |
| --- | ---: | ---: | ---: |
| Referenced HEAD commit | 208 | 2 | 210 |
| Saved worktree | 204 | 2 | 206 |
| Change | -4 | 0 | -4 |

The exact change is **two added errors and six removed errors**, not a
general improvement to the unfinished source circuitry:

- Added on `/Power button and source control/`: `power_pin_not_driven`,
  SYS_AON power symbol #PWR086 pin 1, item UUID
  `d5e7aa3e-e1c5-4c82-8497-bbab5fbd3e4e`.
- Added on the same sheet: `pin_not_connected`, U9.6 EN, item UUID
  `7a0e706e-ea7c-4796-8f79-628cba223927`.
- Removed on `/RA8P1 IO allocation/`: six `pin_not_connected` findings for
  U1.D9 (`c209b822-9d8f-4222-9df3-fc499b8b0662`),
  U1.B6 (`b293fc7b-4096-43c5-aece-92d6557a6748`),
  U1.A12 (`f1044359-ac72-47f3-9ecb-c008f3b68e33`),
  U1.E10 (`08a1ca53-708d-43db-9d4f-7d8fbc7ffcdf`),
  U1.B12 (`715535a6-588d-4ac0-bfc9-4fe993f5d3bc`), and
  U1.B14 (`607a4963-5f08-41bf-9a39-786ca9cc92af`).

Every other ERC identity is unchanged from HEAD. In particular, the
existing three undriven-power findings and three VLO power-output-to-power-
output findings were not created by this button checkpoint. They still
require proper resolution; their pre-existence is not electrical approval.
Do not add PWR_FLAGs, NC markers or ERC exclusions to conceal the missing
SYS_AON source, open EN, or other unfinished connections. Keep these
limitations visible in progress commits and PDF handoffs.

## SYS-001: Power-path and single-button boundary

Use the SYS-006 single-cell candidate with independent pack protection and
an explicitly assembled, cell-contact temperature sensor. A two-wire pack
does not include an NTC. The selected electrical basis does not authorize
charging an unidentified or substituted pack.
The power path is:

```text
USB-C VBUS -> input protection/current-budget control -> charger VBUS
Protected pack positive <----------------------------> charger BAT
Charger SYS -> always-on pushbutton controller
Charger SYS -> enabled buck-boost -> +3V3_MCU -> gated peripherals
```

The exposed button controls the downstream converter, not the battery
connector and not the charger's USB input. Thus a hard-off action can remove
processor power with USB still attached; USB must not bypass the converter
enable. Charging while the application is off remains possible only when
the charger's autonomous settings and thermistor circuit are pack-qualified.
Do not put every always-on device on the switched MCU rail.

Use `SYS_AON` for the charger output, `SYS_EN_REQ` for the button latch
request and `MAIN_PWR_EN` for the qualified converter-enable node. The
candidate open-drain wired-OR circuit can merge the last two electrically
only after its source-valid design is approved. These are interface names,
not claims about current netlist connectivity. `SYS_AON` is not a fixed
3.3 V rail. The main converter and button controller must both tolerate its
complete battery, charging, startup, removal and fault envelope.

A provisional 3.0..4.6 V operating screen is useful for calculations, but
neither endpoint is currently a qualified system limit. The lower cutoff
must exceed the pack's allowed discharge limit with tolerance and load-sag
allowance. The upper bound must include charge-voltage tolerance and SYS
overshoot. An IC's absolute maximum or typical overvoltage threshold is not
a guaranteed output clamp. TPS63802 requires input no higher than 5.5 V in
normal operation; an upstream protection claim must establish that limit.

## SYS-002: Charger comparison and default-state constraints

The imported [BQ25188](https://www.ti.com/lit/ds/symlink/bq25188.pdf),
SLUSFJ3 sections 6.4, 6.5 and 7.3, is linear. Its recommended IN-to-SYS
current is 1.1 A maximum; battery-powered SYS current is 2 A DC or 3 A for
pulses shorter than 20 ms. The 3 A headline is not continuous USB capability.
Its 500 mA default input limit also cannot establish 100 mA USB startup
compliance. The integrated button does not by itself settle product power
behavior, reset defaults or thermal suitability.

Prefer evaluating a switching power-path charger before retaining that
linear part. [BQ25619ERTWR](https://www.ti.com/product/BQ25619E/part-details/BQ25619ERTWR)
is an active, non-OTG candidate with pack supplement and battery-absent SYS
operation. Its [datasheet](https://www.ti.com/lit/ds/symlink/bq25619e.pdf),
SLUSEC9B, Table 6-1, provides this native-symbol connection basis:

| Pin | Function | Proposed connection |
| --- | --- | --- |
| 1, 24 | VAC, VBUS | Protected/current-limited USB input; 1 uF local bypass |
| 2 | PSEL | Defined high startup state; not 2.4 A default |
| 3, 4, 7 | PG, STAT, INT | Open-drain status; switched-domain pullups if used |
| 5, 6 | SCL, SDA | Fail-safe host management interface |
| 8 | NC | Explicit no-connect |
| 9 | CE | Hardware charge inhibit until pack-qualified |
| 10 | BATSNS | Kelvin sense to protected pack positive |
| 11 | TS | Pack-specific thermistor network |
| 12 | QON | Internal service only; no second exposed switch |
| 13, 14 | BAT | Protected pack positive; 10 uF local bypass |
| 15, 16 | SYS | SYS_AON; at least 10 uF effective local bypass |
| 17, 18, exposed pad | Ground | Common ground |
| 19, 20 | SW | Charger inductor to SYS_AON |
| 21 | BTST | 47 nF to SW |
| 22 | REGN | 4.7 uF, 10 V local bypass |
| 23 | PMID | Local input bulk, nominal 2 x 4.7 uF plus 1 nF |

This table is a connection review, not a released BOM. Charge-current and
voltage register defaults, TS thresholds, CE logic, inductor current rating,
effective capacitances and management-pin powered-off behavior must be
resolved before those parts are placed as a qualified circuit. The 1.8 A
recommended SW output-current limit includes current subsequently split
between the system and battery charging; it is not 1.8 A plus charging.
PSEL high means 500 mA, not 100 mA. Charger QON reset with USB present depends
on a register setting that defaults off, so it cannot replace independent
default-state single-button recovery.

Do not solve CE startup by pulling it high to SYS and enabling it only from
a powered MCU without considering hard-off charging: that topology inhibits
charging whenever the application is off. Conversely, a CE pull-down plus
unchecked autonomous charge defaults is unsafe for the unspecified pack.
The final fixed pack and reset-default register values must be checked
together. Keeping charge inhibited is the safe bench state until then.

### SYS-002A: Current and thermal screening

The existing 396.27 mA MCU reference plus 500 mA radio allocation gives:

```text
I_3V3_reference = 0.39627 + 0.500 = 0.89627 A
P_3V3_reference = 3.3 * 0.89627 = 2.957691 W
I_SYS = P_3V3_reference / (eta_main * V_SYS)
```

With an explicitly assumed 90% main-converter efficiency:

| SYS voltage | Reference SYS current |
| --- | --- |
| 3.0 V | 1.095441111 A |
| 3.5 V | 0.938949524 A |
| 4.5 V | 0.730294074 A |

These exclude display, memory, front light, touch, GPIO loading, other
regulators and always-on current. Efficiency is an analysis assumption,
not a guaranteed TPS63802 limit. They are not a full-board maximum.

For a linear charger at VIN = 5 V, SYS = 4.5 V, battery = 3 V and a
hypothetical 0.5 A charge request:

```text
I_IN = 0.730294074 + 0.5 = 1.230294074 A > 1.1 A
P_linear = (5-4.5)*0.730294074 + (5-3)*0.5
         = 1.365147037 W
```

The charge loop would have to reduce current, and temperature could reduce
it further. This is a comparison case, not an allowed pack charge current
or a package-temperature prediction. At SYS = 3.5 V, the switching candidate
has only `1.8-0.938949524 = 0.861050476 A` of its SW-current budget remaining
for all charging and missing SYS loads. It too requires the final budget.

## SYS-003: USB-C power authorization and isolation

Select sink-only, 5 V operation. USB-C does not imply permission to draw
1.5 A or 3 A: observe CC advertisement and the applicable USB data/charging
state. A USB 2.0 standard downstream port's unconfigured allowance is
100 mA; the configured demand must not exceed the declared budget. See the
[USB-IF compliance procedure](https://www.usb.org/sites/default/files/USB-IFTestProc1_3.pdf),
current-draw test, and [USB-IF power guidance](https://compliance.usb.org/index.asp?UpdateFile=Electrical).

Even idealized 5 V / 500 mA input with two assumed 90%-efficient conversion
stages supports only `5*0.5*0.9*0.9/3.3 = 0.613636364 A` at 3.3 V, less
than the existing MCU+radio reference. At 100 mA the figure is 0.122727273 A.
Battery-absent boot therefore needs a low-power startup policy, or hardware
must defer application startup until a sufficient source budget is known.
Battery supplement cannot be invoked when the battery is empty or absent.

[TUSB320LAIRWBR](https://www.ti.com/lit/ds/symlink/tusb320lai.pdf),
SLLSEQ8D Table 1 and sections 7.3/8.2, is a sourced CC-controller candidate.
Its proposed GPIO-mode pin basis is CC1/CC2 = 1/2, PORT = 3 grounded for
sink, VBUS_DET = 4 through a series resistor, ADDR = 5 open for GPIO mode,
OUT3/OUT1/OUT2 = 6/7/8, ID = 9 unused, GND = 10, EN_N = 11 grounded,
VDD = 12. It supplies the CC terminations; do not parallel external Rd
resistors without a deliberate revised circuit.

It is not an input-current limiter or a PD controller. Its non-failsafe
control pins can back-power it when VDD is off. Direct connection between
VBUS-powered GPIO pullups and an unpowered RA8P1 is therefore not approved.
Use a reviewed powered-off-safe interface or always-on hardware decode.
The complete USB current is the charger input plus the CC/controller,
protection and sensing currents, not the charger register alone.

An 887 kohm +/-1%, 100 ppm/C candidate VBUS_DET resistor with a 100 C
temperature-excursion allocation spans 869348.7..904828.7 ohm, within the
855..920 kohm recommended resistor range. This is value screening only;
an exact resistor, contamination allowance and transient rating remain
unselected. Prefer the revised electrical table over an unreviewed 900k
example copied from an older diagram.

The [TPS2553](https://www.ti.com/lit/ds/symlink/tps2553.pdf), SLVS841F
Table 7.5, illustrates a trap: ILIM tied to IN limits output to 50..100 mA,
but the IC's own input current and an upstream CC detector add to that.
It is not an automatic whole-device <=100 mA solution. A final hardware
budget limiter needs margin, default-state proof, suspend behavior, and
review of attach/detach transients. No such limiter is currently approved.

Keep USB VBUS sensing and USB data ESD arrays from back-powering switched
rails. Do not connect probe VTref or a service UART adapter's VCC to the
system power source. Charger reverse blocking addresses its power path;
it does not address these independent signal paths.

## SYS-004: Main buck-boost implementation boundary

[TPS63802 SLVSEU9D](https://www.ti.com/lit/ds/symlink/tps63802.pdf),
Table 7-1, provides the pin mapping: 1 EN = MAIN_PWR_EN; 2 MODE = defined
mode control; 3 AGND and 8 GND = ground; 4 FB = divider midpoint;
5 PG = open-drain status; 6 VOUT = +3V3_MCU; 7 L2 and 9 L1 = opposite
inductor terminals; 10 VIN = SYS_AON. Never substitute the MCU's 2.2 uH
core inductor for this converter's separate nominal 0.47 uH inductor.

Use the 56k/10k precision-divider direction in PWR-002, not the imported
511k/91k example. MODE must not float; a 100k pulldown is a screening
candidate, giving 20 mV with the specified 0.2 uA pin-leakage bound alone.
Board leakage and exact resistor corners must still be added. PWM must be
established before applying the PWM-only voltage bounds to active radio
operation. A 3.6 V high state costs 36 uA nominal in that pulldown.

For VOUT above 2.3 V, the required minimum effective output capacitance is
7 uF. A nominal 22 uF, +/-10%, X7R example would need at least
`7/(22*0.9*0.85) = 0.415923945` remaining capacitance factor before accounting
for additional aging, DC-bias and other effects. This is a requirement on
the selected capacitor's data, not evidence that any 22 uF capacitor meets
it. No unqualified MLCC is approved by that arithmetic.

True shutdown disconnects the converter input from output. It does not
guarantee that the processor rail discharges promptly. With no other load,
an illustrative 100 uF aggregate and the 66k feedback path give:

```text
V(t) = V0 * exp(-t/(R*C))
t(3.3 V -> 0.3 V) = 66000*100e-6*ln(3.3/0.3) = 15.826108800 s
```

An illustrative active 100 ohm discharge path gives 23.978953 ms for the
same capacitance. Neither 100 uF nor 0.3 V is a qualified whole-board
recovery requirement. The final design needs total capacitance, guaranteed
discharge resistance/current, reset/POR thresholds, powered-off injection,
and minimum rearm time. Do not call forced-off a guaranteed cold reboot
until that timing chain is closed.

## Exact candidate sourcing

Snapshot observed 2026-09-07. USD unit prices exclude tax, shipping and any
tariff; stock and price can change. Candidates are not added to the native
BOM until placed and reviewed in KiCad.

| Exact MPN | Source and ordering code | Stock | Qty 1 / 10 / 100 |
| --- | --- | --- | --- |
| BQ25619ERTWR | [DigiKey 296-BQ25619ERTWRCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/BQ25619ERTWR/13545367) | 3784 | 3.04 / 2.277 / 1.8744 |
| BQ25619ERTWR | [Mouser 595-BQ25619ERTWR](https://www.mouser.com/en/ProductDetail/Texas-Instruments/BQ25619ERTWR?qs=T94vaHKWudSMQtJKyPDXWA%3D%3D) | 9481 indexed | 3.04 / 2.28 / 1.88 |
| TPS63802DLAR | [DigiKey 296-TPS63802DLARCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/TPS63802DLAR/10715525) | 14668 | 2.83 / 2.112 / 1.7347 |
| TUSB320LAIRWBR | [DigiKey 296-TUSB320LAIRWBRCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/TUSB320LAIRWBR/5722618) | 16179 | 1.83 / 1.346 / 1.0904 |

The DigiKey IC listings show Active and nine-week standard lead times.
The charger distributor's 1.8 A generic charging field must not override
the manufacturer's 1.5 A charge specification. The TPS63802 datasheet's
Coilcraft 0.47 uH example is not an approved sourcing result: the exact
[XFL4015-471MEC marketplace listing](https://www.digikey.com/en/products/detail/coilcraft/XFL4015-471MEC/21381073)
showed zero stock and no backorders. Distributor substitutes require a new
inductor-loss, saturation and transient review; do not silently accept one.

## SYS-005: Source-valid clamp and undervoltage restart boundary

Historical candidate audit only. Use the SYS-007 implementation direction;
do not place this EN-only topology as the final source-valid circuit.

TPS3840DL31DBVR is a candidate source supervisor, not an approved battery
cutoff. Its [SNVSB03E datasheet](https://www.ti.com/lit/ds/symlink/tps3840.pdf),
sections 5 and 6.5-6.6, specifies the SOT-23-5 mapping: RESET 1, VDD 2,
GND 3, MR 4 and CT 5. VDD monitors SYS_AON. Active-low open-drain RESET
can share the converter EN node with LTC2954's open-drain EN. A push-pull
supervisor variant must not be substituted onto that wired-OR node.
MR may remain open; choose CT only after the required recovery delay is
established. Add local supply bypassing according to the final application.

Nominal 3.1 V falling threshold, +/-1.5% accuracy and 175..225 mV
hysteresis give these conservative limits:

```text
Falling = 3.1*(1 +/- 0.015) = 3.0535..3.1465 V
Rising = falling + hysteresis = 3.2285..3.3715 V
```

These levels leave useful separation from the button IC's 2.7 V operating
minimum but are not proven suitable for an unspecified pack, its voltage
sag, or USB/battery switchover. Audio load steps make that distinction
especially important. A threshold measured at charger SYS is not directly
the same quantity as cell voltage under all power-path operating modes.

For the shared-node candidate, screen R_EN = 470 kOhm with 1% initial
tolerance and 100 ppm/C over 100 C: 460.647..479.447 kOhm. Allocate 2 uA
total adverse external leakage. This is an assumption to be verified at
all relevant voltages, not a guaranteed unpowered-LTC parameter.
The supervisor's low-power-on-reset output test permits 5.6 uA sink at
0.2 V; its controlled-output guarantee is conditioned on supply slew no
faster than 100 mV/us. Ordinary 100 kOhm pullup logic calculations do not
establish this startup condition.

```text
Low-voltage sink screen = (1.5-0.2)/460.647k + 2 uA = 4.822118 uA
EN high screen = 3.0535 - 2 uA*479.447k = 2.094606 V
EN rising margin = 2.094606 - 1.13 = 0.964606 V
```

By contrast 100 kOhm with the same tolerance/leakage allocation needs
15.263953 uA in that low-voltage screen. BTN-008's 100 kOhm calculation
therefore demonstrates direct controller/converter logic compatibility,
not compatibility with this particular supervisor at low supply voltage.
The converter's EN-specific thresholds, not the generic VIH/VIL row,
give 0.57 V falling margin to the controller's 0.4 V maximum low.

The 470 kOhm screen is not whole-interface approval. Bound LTC EN leakage
below its 2.7 V operating minimum, off-state input paths, pullup-node
capacitance and the actual SYS ramp. The supervisor's normal operating
range starts at 1.5 V; its special low-voltage output guarantee must be
applied using its own test conditions. Do not treat a fast battery-contact
step as a verified slow ramp.

An EN-only supervisor can restart the converter after a discharged battery
rebounds. The button latch may still be on if KILL has not fallen. A
product requiring a new press after undervoltage must either independently
clamp KILL to clear the latch, or prove that discharge and the supervisor's
minimum release delay always clear KILL before EN is released. Include
the button IC's turn-on blanking interval in that proof. Do not directly
tie KILL to EN: their pullups belong to different power domains. The
firmware cannot be the only mechanism stopping a brownout/restart loop.
Passive discharge and a nominal CT delay are not sufficient evidence.

```python
from math import isclose

fall_min, fall_max = 3.1*0.985, 3.1*1.015
rise_min, rise_max = fall_min+0.175, fall_max+0.225
rmin, rmax = 470000*0.99*0.99, 470000*1.01*1.01
leak_allocation = 2e-6  # Conditional design allocation, not an IC guarantee.
low_sink = (1.5-0.2)/rmin + leak_allocation
en_high = fall_min-leak_allocation*rmax
assert isclose(fall_min, 3.0535) and isclose(fall_max, 3.1465)
assert isclose(rise_min, 3.2285) and isclose(rise_max, 3.3715)
assert low_sink < 5.6e-6
assert isclose(en_high, 2.094606) and en_high > 1.13
sink_100k = (1.5-0.2)/(100000*0.99*0.99)+leak_allocation
assert sink_100k > 5.6e-6
print("SYS-005 falling / rising V", (fall_min, fall_max), (rise_min, rise_max))
print("SYS-005 low sink uA / EN high V", low_sink*1e6, en_high)
print("SYS-005 arithmetic PASS; leakage/ramp/restart assumptions remain open.")
```

The Python block was executed successfully. Procurement, the exact pack
cutoff and the complete latched-fault/discharge implementation must be
closed before this section becomes an approved native circuit.

## Reproducible arithmetic

Run from the worktree root. This checks the numerical statements above,
not the BOM or schematic, since these candidate blocks are not placed.

```sh
python3 - <<'PY'
from math import isclose, log

p = 3.3 * (0.39627 + 0.500)
checks = {
    'reference power W': (p, 2.957691),
    'SYS 3.0 V current A': (p/0.9/3.0, 1.095441111111111),
    'SYS 3.5 V current A': (p/0.9/3.5, 0.9389495238095238),
    'SYS 4.5 V current A': (p/0.9/4.5, 0.7302940740740741),
    'linear input A': (p/0.9/4.5+0.5, 1.2302940740740742),
    'linear loss W': ((5-4.5)*p/0.9/4.5+(5-3)*0.5, 1.365147037037037),
    'remaining SW current A': (1.8-p/0.9/3.5, 0.8610504761904763),
    '100 mA USB output A': (5*0.1*0.9*0.9/3.3, 0.12272727272727274),
    '500 mA USB output A': (5*0.5*0.9*0.9/3.3, 0.6136363636363636),
    'VBUS resistor minimum ohm': (887000*0.99*0.99, 869348.7),
    'VBUS resistor maximum ohm': (887000*1.01*1.01, 904828.7),
    'MODE leakage-only voltage V': (0.2e-6*100e3, 0.02),
    'MODE pulldown nominal current A': (3.6/100e3, 36e-6),
    'remaining capacitance factor': (7/(22*0.9*0.85), 0.41592394533571003),
    'passive decay s': (66000*100e-6*log(11), 15.826108800469248),
    'illustrative active decay s': (100*100e-6*log(11), 0.023978952727983706),
}
for name, (actual, expected) in checks.items():
    if not isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-12):
        raise ValueError((name, actual, expected))
    print(f'{name}: {actual:.12g}')
if not 855000 <= checks['VBUS resistor minimum ohm'][0]:
    raise ValueError('VBUS detector resistor too low')
if not checks['VBUS resistor maximum ohm'][0] <= 920000:
    raise ValueError('VBUS detector resistor too high')
print('SYS-001..004 arithmetic PASS; qualification limits remain explicit.')
PY
```

## Release inputs that cannot be inferred

SYS-006 supplies a specific pack electrical basis, but the ordered pack
revision, charge-voltage tolerance, precharge/termination limits, completed
temperature-sensor harness and connector remain release inputs. A generic
10k resistor is not a substitute for a thermistor contacting the cell.
Never enable TS_IGNORE to bypass missing pack temperature sensing.

Panel/controller rail loads and warm/cool LED electrical limits are needed
to approve total converter capacity and runtime. They cannot be deduced
from pixel count or the number of LEDs. Product low-battery cutoff and
charger defaults must also be reviewed against the actual pack. These
unknowns prevent electrical release but do not prevent completing unrelated
host/radio circuitry or the independent single-button control section.

## SYS-006: Selected battery electrical basis and audio power path

Select Jauch `LP906090JH+PCM+2 WIRES 70MM`, Jauch material 246525, as the
electrical battery candidate. This is a purchased protected pack, not a
request to assemble cells or alter its protection circuit. The
[manufacturer's August 2024 revision 1.1](https://www.jauch.com/downloadfile/677e3f68c583d30f0e3fd6874fe248710/matd_246525_lp906090jh.pdf)
specifies 6 Ah minimum at its stated 0.2C discharge condition, 3.7 V
nominal, 4.2 V charge, 1.2 A standard/3 A maximum charge and 6 A maximum
discharge. Pack discharge cutoff is 3.0 V; PCM undervoltage detection is
3.00 +/-0.05 V. The PCM has a second protection IC. Its 7..15 A
overcurrent trip is fault protection, not permission for that load.

The catalog charging window is 0..45 C and discharge window -20..60 C.
The [older 2022 pack document](https://www.jauch.com/downloadfile/5bf529a732a3120f6edf04743f55a5dcb/6000mah_-_lp906090jh_1s1p_2_wire_70mm.pdf)
has different temperature and protection limits. Purchasing must link the
lot to the governing manufacturer revision. The earlier 10..40 C actual-cell
target is superseded by SYS-008's tolerance-bounded sensor window and
explicit sensor-to-cell error allocation, checked against this 2024
revision's 0..45 C limit. A 2022-revision pack is not approved by that proof.

This stocked SKU has **two wires and no thermistor**. SYS-008 selects
Semitec `104JT-025` instead of the earlier `103AT-2` candidate.
Its installation must contact the pack surface through an electrically
insulating, manufacturer-compatible mechanical sensor fixture. Do not
open, solder to, encapsulate or modify the pouch. A PCB ambient NTC is not
equivalent. Sensor open/short must inhibit charge independently of host
software. The enclosure contact pressure, thermal lag, wire strain relief
and a keyed harness rated for the selected pack current need drawings.
Do not use a 2 A JST-PH connection for a proposed 4 A battery path.

SYS-008 selects 1.120861 A nominal, 1.187930 A upper calculation corner,
and autonomous 4.10 V regulation instead of the earlier 4.16 V candidate.
USB authorization, system demand and temperature can reduce charging.
This is not permission to select a nominal 4.2 V charger with an unchecked
upper tolerance. Capacity
at reduced termination voltage and at the actual cutoff/load is not the
catalog 6 Ah test result. `3.7*6 = 22.2 Wh` is nominal nameplate energy,
not a runtime guarantee.

Reserve a **4 A continuous battery-path engineering allocation** initially,
below the pack's 6 A discharge limit. This is an allocation, not an assertion
that the present schematic draws 4 A or that every missing load fits.
The host/radio plus baseline headphone screen already consumes
`3.286323333 + 2.892695048 = 6.179018381 W` before DAC, camera, display,
memory, front light and other loads. At 3 V that is 2.059672794 A. Retaining
the SYS-002 BQ25619E 1.8 A switching-path screen for the expanded design is
therefore not approved.

The now-superseded `BQ25638YBGR` evaluation candidate's
[SLUSF18B datasheet](https://www.ti.com/lit/ds/symlink/bq25638.pdf) describes
5 A switch-mode charging, NVDC system power path, programmable input limit
and hardware ILIM. Do not interpret the headline as 5 A USB-C entitlement
or infer that a battery-current fault threshold equals a continuous rating.
The exact BATFET/SW recommendations, inductor, all defaults, JEITA remap,
4.16 V code, autonomous hard-off charging, 100 mA USB startup budget and
powered-off I2C interface still require a dedicated charger connection
review. `BQ25895RTWR` is an in-stock QFN fallback, not a drop-in replacement;
its automatic D+/D- detection must not conflict with RA8P1 USB-HS data.
Do not place either candidate with charge enabled before that review.
SYS-008 chooses BQ25616 because its relevant defaults are set by resistors
and its charging temperature protection does not require the switched MCU.

Snapshot, 2026-09-07; USD, excluding taxes, shipping and tariffs:

| Exact item | Stock / unit prices | Source |
| --- | --- | --- |
| Jauch LP906090JH+PCM+2 WIRES 70MM | 2652; 1/$30.08, 10/$25.022, 81/$21.17469 | [DigiKey 1908-LP906090JH+PCM+2WIRES70MM-ND](https://www.digikey.com/en/products/detail/jauch-quartz/LP906090JH-PCM-2-WIRES-70MM/9560999) |
| Semitec 103AT-2 | Candidate stocked sensor; refresh quote with harness release | [DigiKey 4316-103AT-2-ND](https://www.digikey.com/en/products/detail/semitec-usa-corp/103at-2/16579059) |
| BQ25638YBGR | 4505; 1/$4.47, 10/$3.38, 100/$2.81 | [Mouser 595-BQ25638YBGR](https://www.mouser.com/en/ProductDetail/Texas-Instruments/BQ25638YBGR?qs=mELouGlnn3erNLXwtKCMiQ%3D%3D) |
| BQ25895RTWR | 3394; 1/$3.36, 10/$2.52, 100/$2.0804 | [DigiKey 296-44345-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/BQ25895RTWR/6110618) |

## SYS-007: Held-up control, source-fault latch clear and main discharge

This is the selected **implementation direction**, replacing SYS-005's
EN-only clamp. It is a native-schematic connection specification, not a
claim of existing placement, ERC completion or bench qualification.
Use with [BTN-010](single_button_power.md); the source-fault circuit adds
no exposed switch. There are five product keys, only one of which controls
the power latch. High-quality audio, speakers, camera and full memory
remain in scope; their high-current supply paths do not pass through the
control hold-up components.

### Connections

```text
SYS_AON --- LM66100 VIN/VOUT --- 100 ohm ---> AON_HOLD
     |          | CE                             | 4 x 100 uF / 10 V
     |          +--------------------------------+
     +---- TPS3808 SENSE divider                  +-- LTC2954 VIN
     +---- main/audio/display converter VIN      +-- TPS3808 VDD
                                                 +-- 74LVC1G14 VCC

AON_HOLD -- 100k -- MAIN_PWR_EN -- TPS63802 EN
                        |  |  |
                 LTC EN   |  +-- inverter --> POWER_OFF_H
                  TPS3808 RESET                   |       |
                                                  |       +-- Qkill gate
                                                  +-- Qdischarge gate
Qkill: drain = POWER_KILL_N; source = GND
Qdischarge: drain = (22 ohm || 22 ohm) to +3V3_MCU; source = GND
```

The inverter drives two gates, not the rails themselves. EN and KILL are
separate electrical nets with separate supply-domain pullups. Their logical
relationship uses the LTC's documented startup KILL blanking; do not replace
the inverter/FET relationship with a wire, diode or common pullup.

| Part/pin | Required connection |
| --- | --- |
| LM66100DCKR 1 VIN | Raw SYS_AON directly; C62 100 nF local bypass to GND |
| LM66100 2 GND, 3 CE | GND; CE tied to AON_HOLD downstream of R31, respectively |
| LM66100 4 NC, 5 ST, 6 VOUT | Internally unconnected NC; unused ST tied to GND per pin table; VOUT through R31 100 ohm to AON_HOLD |
| TPS3808G01DBVR 1 RESET | MAIN_PWR_EN, shared with LTC EN and converter EN |
| TPS3808 2 GND, 3 MR | GND; MR tied to AON_HOLD |
| TPS3808 4 CT | 3 x 100 nF C0G in parallel to GND, not 300 ms fixed mode |
| TPS3808 5 SENSE | 732k from raw SYS_AON, 100k to GND, 1 nF C0G to GND |
| TPS3808 6 VDD | AON_HOLD, local 100 nF bypass |
| Nexperia 74LVC1G14GW 1/2/3/4/5 | NC / MAIN_PWR_EN / GND / POWER_OFF_H / AON_HOLD |
| Nexperia 74LVC1G14 local bypass | 100 nF directly between VCC pin 5 and GND pin 3 |
| Two DMN2056U-7, G/S/D = 1/2/3 | Each gate via its own 100 ohm from POWER_OFF_H; each gate has 1M to GND; source GND; drains as diagram |
| LTC2954 VIN and local bypass | AON_HOLD, not raw SYS_AON |
| LTC PB external 10k pullup | Raw SYS_AON; preserve BTN series-1k/100n/contact/ESD circuit |
| LTC INT and KILL pullups | Switched +3V3_MCU; preserve BTN KILL 10k/100k bias and P903 open-drain request |

Pin references are from [LM66100 Rev A](https://www.ti.com/lit/ds/symlink/lm66100.pdf),
[TPS3808 Rev N](https://www.ti.com/lit/ds/symlink/tps3808.pdf),
[Nexperia 74LVC1G14 Rev 19.1](https://assets.nexperia.com/documents/data-sheet/74LVC1G14.pdf)
and [DMN2056U](https://www.diodes.com/datasheet/download/DMN2056U.pdf).
The selected Nexperia gate's 1 uA input-leakage bound matters: the TI
SN74LVC1G14 5 uA bound must not be silently substituted into this network.

### Operating sequence and fault policy

1. After a fresh source connection, TPS3808 holds EN low during control-rail
   startup and its source-qualification delay. The inverter consequently
   keeps the KILL clamp and main-rail discharge on.
2. With no qualification-period press, LTC2954 still requests off when
   source qualification finishes. A fresh power-key press releases EN,
   turns both FETs off and starts the converter.
   MCU-rail KILL bias must rise above 0.68 V before the LTC's 400 ms minimum
   startup blank ends. Firmware may later pull KILL low to shut down.
3. Low raw SYS asserts the supervisor even while AON_HOLD remains valid.
   EN falls, the converter stops, and both KILL and rail discharge activate.
   The supervisor remains asserted after voltage recovery long enough to
   outlast the LTC's 650 ms maximum initial KILL blank. This clears the
   latch even if the undervoltage happened immediately after turn-on.
4. Battery rebound releases the supervisor only after qualification; it
   does not create a fresh power-key press. The LTC remains latched off.
   Complete control-power loss also returns the LTC to its initial off state.

A press during source qualification can be aborted if KILL blanking expires
before source release. A late press can instead leave the LTC internally on
until the supervisor releases EN, producing a short delayed start. The
product accepts that delayed execution of an intentional press; this circuit
does not require or enforce a release-and-repress sequence. A source fault
without a new press must still clear the previous on latch. Qualification
must test presses at both ends of the reset-delay interval and repeated
fault/recovery transitions, not just steady states.

The running cutoff below is not a cold-restart guarantee. LTC2954 specifies
a falling UVLO upper limit of 2.5 V and hysteresis upper limit of 0.7 V;
their conservative sum requires 3.20 V at its VIN for guaranteed UVLO
release. The 3.0137 V held-rail minimum at the running cutoff is insufficient
for that claim. The successful-recovery operating contract is therefore
raw SYS_AON at least 3.70 V, AON_HOLD at least 3.25 V, and **both** the
1 s reservoir-settling allowance and the full source-qualification interval
completed before a new press. The nominal supervisor delay alone is
1.715 s; waiting only 1 s is not a successful-start instruction. Establish
the total maximum recovery wait in qualification; the shared EN node is
held low by the off-state LTC and is not a separately readable source-valid
signal. Under the declared 1.125 mA control-load allocation, a deliberately
conservative charging model subtracts 250 mV continuously and includes
the 0.23 ohm installed switch-resistance allocation:

```text
Rpath_max = 102.01 + 0.23 = 102.24 ohm
Vhold_steady_model = 3.70 - 0.250 - 102.24*0.001125 = 3.334980 V
C_hold_max_model = 400u * 1.1 * 1.1 * 1.1 = 532.4 uF
t_to_3.25_model = Rpath_max*Cmax*ln(Vsteady/(Vsteady-3.25)) = 199.757 ms
```

This RC charging model is more pessimistic than the static comparator/path
bound used for hold-up below; it does not treat the turn-on threshold as an
additional physical series diode. The 1 s reservoir allowance must also cover actual switch
startup and qualified capacitor leakage, while supervisor qualification
is a separate wait requirement. This is a recovery/test
contract, not an added voltage comparator or a guaranteed-off threshold:
the present circuit may start below 3.70 V. Below that contract, successful
restart is unspecified; charge the approved pack and retry. Do not promise
USB-only cold start with a missing/depleted pack merely because SYS crosses
the lower running threshold. Charging while application power is off
remains independent.

### Threshold, logic and timing calculations

TPS3808G01 uses a nominal 0.405 V threshold with +/-2% limit, not the fixed
version's headline accuracy. Both divider resistors are +/-0.1%, 25 ppm/C;
the corner analysis allocates 100 C temperature excursion independently.
Include the SENSE +/-25 nA input current:

```text
Vtrip = Vref * (1 + Rtop/Rbottom) + Isense * Rtop
Vtrip_nominal = 0.405 * (1 + 732k/100k) = 3.369600 V
Vtrip_corners = 3.263705831 .. 3.476597632 V
```

The nominal cutoff is deliberately above the pack's 3.0 V endpoint.
Runtime must be qualified at the actual load/sag, not at an assumed relaxed
cell voltage. TPS3808 hysteresis has no listed nonzero minimum for G01;
restart safety uses latch clear plus delay, not an invented hysteresis bound.

The divider selections are R32 `RT0805BRD07732KL` (732 kOhm) and R33
`RT0603BRD07100KL` (100 kOhm). The
[YAGEO RT series specification, pp. 5-6](https://yageogroup.com/content/datasheet/asset/file/PYU-RT_1-TO-0-01_ROHS_L)
rates RT0805 at 0.125 W and 150 V maximum working voltage, and RT0603 at
0.100 W and 75 V. Rated power applies at 70 C; derate above 70 C according
to the published curve. The
[R33 exact-part specification](https://www.yageogroup.com/component-documentation/download/specsheet/RT0603BRD07100KL)
also confirms 0.100 W, 75 V, +/-0.1% and +/-25 ppm/C. The RT ordering code
and electrical table establish the same tolerance/TCR for R32. Continuous
voltage must meet both the power-derived limit `sqrt(P_allowed*R)` and the
package working-voltage ceiling; the quoted overload voltage is not a
continuous rating.

For a conservative DC stress screen within the declared 0..4.6 V node
envelope, apply the entire 4.6 V across each resistor individually instead
of relying on the nominal divider ratio or extending the SENSE current
specification away from its threshold test point:

```text
Rmin_factor = (1-0.001)*(1-25ppm/C*100C) = 0.9965025
P_R32_screen = 4.6^2/(732k*Rmin_factor) = 29.008561 uW
P_R33_screen = 4.6^2/(100k*Rmin_factor) = 212.342668 uW
V_each_screen <= 4.6 V < 75 V < 150 V
```

These bounds are well below 125 mW / 100 mW at 70 C; actual divider
dissipation is lower. This is a source-envelope calculation, not surge or
fault qualification, and does not approve operation at arbitrary ambient
temperature or an unqualified SYS_AON overvoltage.

Allocate 3 uA total adverse EN leakage: LTC 1 uA high-voltage-test bound,
supervisor 0.3 uA, Nexperia input 1 uA, TPS63802 0.2 uA and 0.5 uA board
allowance. The 100k pullup is +/-1%, 100 ppm/C in this conservative screen.
At AON_HOLD = 2.7 V, EN high is at least 2.393970 V, above TPS63802's
1.13 V maximum rising threshold. At EN low <=0.4 V, the converter's
0.97 V minimum falling threshold leaves 0.57 V margin. For the inverter,
0.4 V is below its 0.65 V minimum falling threshold at the lower 2.3 V
table point; 2.393970 V exceeds its 1.71 V maximum rising threshold at
the upper 3 V point. Verify the continuous rail sweep during qualification.

At 1.3 V held supply, the POR sink screen is
`(1.3-0.2)/98010 + 3u = 14.223344557 uA`, below TPS3808's 15 uA POR test
condition. Below rated LTC VIN, its EN leakage is not separately guaranteed;
the cold-start test must cover the actual combined circuit. The large hold
reservoir and charging resistor make the supervisor supply rise much slower
than its required 15 us/V minimum rise time.

At AON_HOLD >=2.7 V, the inverter specifies output high >=VCC-0.1 at
100 uA load. Two 1M gate pulldowns plus gate leakage remain below that load;
therefore gate drive is >=2.6 V after settling. DMN2056U has a 2.5 V gate
RDS(on) test point. Use **0.2 ohm maximum installed discharge-path FET
resistance** as a qualification allocation, not the 25 C 45 milliohm value
as an all-temperature promise. The same allocation makes the KILL clamp
drop less than 76 uV at the BTN 0.37931 mA sink screen.

For CT use three `GRM31C5C1H104JA01K` 100 nF, +/-5%, 50 V C0G capacitors.
The [Murata reference specification](https://www.mouser.com/datasheet/2/281/1/GRM31C5C1H104JA01_01A-1987788.pdf)
provides the part basis. C0G avoids an unreviewed X7R bias/aging timing term.
Printed p. 1 specifies +/-30 ppm/C from 25 C to 125 C, not a single linear
bound for every cold temperature. Printed p. 5, Table A, instead allows
capacitance changes of -0.24%..+0.58% at -55 C, -0.17%..+0.40% at -30 C,
and -0.11%..+0.25% at -10 C. Use an initial tolerance/temperature screen
of -0.30%..+0.58% to encompass the hot TC range and these published cold
bounds. This corrects the former 315.945 nF upper screen:

```text
Ctotal_min = 3*100nF*0.95*(1-0.003) = 284.145 nF
Ctotal_max = 3*100nF*1.05*(1+0.0058) = 316.827 nF
```

TI's nominal equation is `td = C(nF)/175 + 0.0005 s`: nominal 1.714786 s.
A +/-40% IC-delay model, consistent with the published 180 nF timing row,
gives a 0.974511 s minimum and 2.535316 s maximum screen. This proportional
model is **not** a new guaranteed min/max specification at every
capacitance. The initial capacitor envelope does not include soldering,
endurance, leakage or board contamination; qualify those effects rather
than treating the calculation as an end-of-life guarantee. Require a
measured/validated reset hold >=0.90 s across corners, exceeding 650 ms
blanking plus a 10 ms KILL-recognition/control allowance. The older fixed
300 ms mode is rejected for this early-brownout behavior.

### Reservoir and discharge calculation

Use four polarized `T491D107K010AT` in parallel. Their 10 V rating keeps
the present 4.6 V SYS ceiling below 50% rated voltage. The
[KEMET T491 specification](https://content.kemet.com/datasheets/KEM_T2005_T491.pdf)
gives +/-10% tolerance, +/-10% temperature change through 85 C, +/-10%
endurance capacitance change and temperature-dependent leakage. A
conservative stacked screen is `C_hold_min = 400u * 0.9^3 = 291.6 uF`.
Stacking 10 uA initial leakage, 10x at 85 C and 1.25x endurance gives a
500 uA bank-leakage screen. This is not an unlimited-life qualification.

Allocate **1.125 mA control-island current**, including capacitor leakage,
LTC supply and low-PB internal bias, supervisor, EN/gate pullups, inverter
static/non-rail input current and reverse-blocker leakage. The 500 uA
inverter additional-current test condition is included; transition charge
needs separate reserve. LTC PB current at exactly 0 V is not bounded by
its 15 uA specification at PB=0.6 V. Characterize/obtain a valid bound for
that condition and include it in the 1.125 mA limit; do not silently ignore it.
This adds 125 uA for the fourth reservoir capacitor to the previous 1 mA
control-load allocation. It excludes the separately budgeted reverse current
through R31 while the comparator has not yet switched off.

The rejected topology put R31 before U10 VIN and tied CE to immediate VOUT.
With U10 already on and raw SYS grounded, R31 limited reverse current to
about 29 mA at a 2.912 V reservoir. Across the typical 91 milliohm switch,
that produced only 2.65 mV, below the typical 35 mV turn-off threshold.
The switch could stay on and drain the reservoir through R31; the former
three-capacitor minimum model reached 2.7 V in about 1.6 ms. A 1 uC switching
reserve could not account for that continuous discharge. The former
45.297878 ms result is rejected with that topology.

The corrected circuit connects VIN directly to raw SYS, puts R31 after
VOUT, and senses CE at AON_HOLD after R31. This is the output-resistor
workaround described in [TI's reverse-blocking explanation](https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1088112/lm66100-how-the-rcb-circuit-works).
The comparator now senses the full raw-to-reservoir difference, including
R31's drop. Before turn-off, its 80 mV maximum threshold permits reverse
current up to `0.080/98.01 = 0.816243 mA`; allocate **0.817 mA** throughout
the hold interval, independently of the 1.125 mA control load. Comparator
and switch latency require a separate charge reserve. The total continuous
hold-current allocation is therefore **1.942 mA**.

For the settled initial-voltage bound, use the larger of the 250 mV
turn-on threshold magnitude and the control-load resistive drop. These
are alternative limiting conditions with CE after R31, not additive
series drops: when off, the reservoir must fall to the turn-on threshold;
when on, its settled drop is the control current times the path resistance.
Use 0.23 ohm maximum installed switch resistance as an allocation covering
the operating range, based on the datasheet's 1.8 V maximum table point.
Qualify it across the continuous supply sweep. Finite turn-on delay can
cause initial undershoot; include that loss in the shared charge reserve.

```text
Rpath_max = 102.01 + 0.23 = 102.24 ohm
Vhold_initial_static_min = 3.263705831 - max(0.250, 0.001125*102.24)
                        = 3.013705831 V
Qreserve = 1 uC (initial undershoot plus reverse-switching and gate/logic losses)
thold_to_2.7 = [291.6u*(3.013705831-2.7) - 1u] / 1.942m
             = 46.589403 ms
```

LM66100's 2 us reverse turn-off figure is typical, not a guaranteed upper
limit. The 1 uC combined reserve is an explicit test requirement, not a
derived guarantee, and must not be spent independently at each transition.
Raw-SYS removal, slow threshold-region ramps, floating-source removal and
repeated fault/recovery transitions must demonstrate adequate AON_HOLD
voltage throughout discharge and satisfy the total charge/current bounds.

[TI's low-input clarification](https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1253466/lm66100-oring-with-discrete-mosfet-behavoiur-with-no-power-on-vin-or-ce-pins)
states that CE can power the device when CE exceeds VIN by 80 mV, including
a disconnected VIN. This supports the corrected collapse behavior, but
the datasheet does not explicitly guarantee the quoted leakage limits at
VIN = 0 V; its normal input operating range starts at 1.5 V. Another
[TI leakage discussion](https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1354894/lm66100-shut-down-or-leakage-current)
explicitly cautions against treating VIN = 0 V as normal operation.
Blocking, CE supply current and reverse leakage across raw VIN = 0..4.6 V
therefore need vendor confirmation or qualification within the declared
budgets. No guaranteed 0 V leakage figure is inferred from typical plots
or the default 3.6 V electrical-characteristics test condition. Include PB
leakage into an unpowered controller during recovery. Do not add audio,
LED, MCU or other power loads to AON_HOLD without redoing this energy budget.

The switched main rail is allocated **no more than 1 mF total effective
capacitance**, including attached peripherals that remain electrically
connected during shutdown. This is a design limit to reconcile with the
actual capacitor inventory, not the inventory itself. For two 22 ohm,
1 W, +/-1%, 100 ppm/C resistors in parallel and 0.2 ohm FET allocation:

```text
Rdischarge_max = (22*1.01*1.01)/2 + 0.2 = 11.4211 ohm
t(3.6 V -> 0.3 V) = Rmax * 0.001 * ln(12) = 28.380367 ms
P_each_initial_max = 3.6^2 / (22*0.99*0.99) = 0.601052 W
E_total_initial = 0.5 * 0.001 * 3.6^2 = 6.48 mJ
```

Allow at most 10 ms for source detection, EN propagation, converter energy
decay and establishment of discharge. The resulting 38.380367 ms screen
fits the 46.589403 ms hold screen with 8.209035 ms margin and the LTC 200 ms
minimum rearm interval.
These are engineering allocations to verify together, not independently
interchangeable datasheet guarantees. Repeated-insertion, partially charged
reservoir and resistor-temperature tests are mandatory. A shorted reservoir
is current-limited by 100 ohm; at 4.6 V its worst resistor power is 0.215896 W.

Audio positive/negative rails and panel high-voltage rails need their own
local mute, power-off sequencing and discharge paths. MAIN_PWR_EN low must
force the audio jack disconnect/default mute and disable rail enables even
with USB attached. Do not assume the main 3.3 V bleeder drains isolated
boost outputs or provides panel-safe shutdown ordering. A booted processor
must not drive powered-off peripherals through their I/O.

### Candidate construction BOM

Snapshot 2026-09-07. All IC/hold-cap candidates below were listed Active;
the table is a construction basis, not an assertion that KiCad's BOM has
already been updated. Native schematic fields and exported BOM must match
when the circuit is placed. Bypass/gate-bias small passives may reuse
qualified project parts at the specified values.

| Qty | Exact part / function | Source; observed stock and USD qty-1 price |
| --- | --- | --- |
| 1 | LM66100DCKR, hold isolation | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/LM66100DCKR/10273183); 28390, $0.32 |
| 1 | TPS3808G01DBVR, source supervisor | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TPS3808G01DBVR/666712); 101626, $1.96 |
| 1 | 74LVC1G14GW,125, OFF_H inverter | [DigiKey](https://www.digikey.com/en/products/detail/nexperia-usa-inc/74LVC1G14GW-125/946729); 316674, $0.10 |
| 2 | DMN2056U-7, discharge/KILL FETs | [DigiKey](https://www.digikey.com/en/products/detail/diodes-incorporated/DMN2056U-7/7352909); 80160, $0.39 |
| 4 | T491D107K010AT, 100u/10V hold | [DigiKey](https://www.digikey.com/en/products/detail/kemet/T491D107K010AT/818629); placement refresh: 11863, $1.64; $1.09/0.8033 at 10/100 |
| 3 | GRM31C5C1H104JA01K, CT 100n C0G | [DigiKey](https://www.digikey.com/en/products/detail/murata-electronics/GRM31C5C1H104JA01K/2548138); 70095, $0.52 |
| 1 | RT0805BRD07732KL, 732k divider top | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RT0805BRD07732KL/6617094); 15646, $0.10 |
| 1 | RT0603BRD07100KL, 100k divider bottom | [DigiKey YAG1235CT-ND](https://www.digikey.com/en/products/detail/yageo/RT0603BRD07100KL/1072187); placement refresh: 39669, $0.10; $0.067/0.0559 at 10/100 |
| 2 | RC2512FK-0722RL, main discharge | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RC2512FK-0722RL/5922011); 9772 indexed, $0.31 |
| 1 | RC2512FK-07100RL, hold charging resistor | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RC2512FK-07100RL/5921799); placement refresh: 3845, $0.33 |

### Python verification for SYS-006/007

Run this block with Python 3. It verifies arithmetic and the explicitly
stated allocations, not physical performance or manufacturer limits that
the source documents do not guarantee.

```python
from itertools import product
from math import isclose, log

precision_corners = ((1-.001)*(1-.0025), (1+.001)*(1+.0025))
divider_top_power_screen = 4.6**2/(732e3*precision_corners[0])
divider_bottom_power_screen = 4.6**2/(100e3*precision_corners[0])
trips = [
    ref*(1+732e3*top/(100e3*bottom)) + leakage*732e3*top
    for ref, top, bottom, leakage in product(
        (.405*.98, .405*1.02), precision_corners,
        precision_corners, (-25e-9, 25e-9)
    )
]
vtrip_min, vtrip_max = min(trips), max(trips)
rmin_factor, rmax_factor = .99*.99, 1.01*1.01
chold_min = 4*100e-6*.9*.9*.9
chold_max_model = 4*100e-6*1.1*1.1*1.1
icontrol_alloc = .001125
ireverse_threshold = .080/(100*rmin_factor)
ireverse_alloc = .000817
ihold_alloc = icontrol_alloc + ireverse_alloc
rpath_max = 100*rmax_factor + .23
vhold_min = vtrip_min - max(.25, rpath_max*icontrol_alloc)
vrestart_steady = 3.7 - .25 - rpath_max*icontrol_alloc
trestart_model = rpath_max*chold_max_model*log(
    vrestart_steady/(vrestart_steady-3.25))
qtransient_alloc = 1e-6
hold = (chold_min*(vhold_min-2.7)-qtransient_alloc)/ihold_alloc
rdischarge_max = 22*rmax_factor/2 + .2
discharge = rdischarge_max*.001*log(12)
ct_min_nf = 3*100*.95*(1-30e-6*100)
ct_max_nf = 3*100*1.05*(1+.0058)  # Murata Table A cold upper bound
checks = {
    'battery nominal energy Wh': (3.7*6, 22.2),
    'selected charge upper V': (4.1*1.004, 4.1164),
    'trip nominal V': (.405*(1+732e3/100e3), 3.3696),
    'trip lower V': (vtrip_min, 3.263705830523478),
    'trip upper V': (vtrip_max, 3.4765976320231147),
    'divider top full-source power screen W': (divider_top_power_screen, 29.008561268172036e-6),
    'divider bottom full-source power screen W': (divider_bottom_power_screen, 212.3426684830193e-6),
    'hold C minimum F': (chold_min, .0002916),
    'hold C maximum model F': (chold_max_model, .0005324),
    'capacitor bank leakage screen A': (4*10e-6*10*1.25, .0005),
    'reverse threshold current A': (ireverse_threshold, .0008162432404856647),
    'total hold current allocation A': (ihold_alloc, .001942),
    'hold initial static minimum V': (vhold_min, 3.013705830523478),
    'restart steady model V': (vrestart_steady, 3.33498),
    'restart charge model s': (trestart_model, .19975699545053444),
    'hold interval s': (hold, .04658940277067255),
    'hold shutdown margin s': (hold-discharge-.010, .00820903543277882),
    'discharge resistance ohm': (rdischarge_max, 11.4211),
    'discharge interval s': (discharge, .02838036733789373),
    'each discharge resistor peak W': (3.6**2/(22*rmin_factor), .6010518407212623),
    'rail stored energy J': (.5*.001*3.6**2, .00648),
    'EN high lower V': (2.7-3e-6*100e3*rmax_factor, 2.39397),
    'POR sink A': ((1.3-.2)/(100e3*rmin_factor)+3e-6, 14.223344556677891e-6),
    'CT minimum nF': (ct_min_nf, 284.145),
    'CT maximum nF': (ct_max_nf, 316.827),
    'CT nominal delay s': (300/175+.0005, 1.7147857142857144),
    'CT minimum delay model s': (.6*(ct_min_nf/175+.0005), .9745114285714285),
    'CT maximum delay model s': (1.4*(ct_max_nf/175+.0005), 2.535316),
}
for name, (actual, expected) in checks.items():
    assert isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-12), (name, actual, expected)
    print(f'{name}: {actual:.12g}')
assert vtrip_min > 3.05
assert divider_top_power_screen < .125  # rated power at 70 C
assert divider_bottom_power_screen < .100  # rated power at 70 C
assert 4.6 < 75 < 150  # both selected continuous working-voltage ceilings
assert ireverse_alloc > ireverse_threshold
assert isclose(icontrol_alloc, .001 + .000125, rel_tol=1e-12)
assert hold > discharge + .010
assert vrestart_steady > 2.5 + .7
assert trestart_model < 1.0
assert discharge + .010 < .200
assert checks['POR sink A'][0] < 15e-6
assert checks['EN high lower V'][0] > 1.13
assert checks['CT minimum delay model s'][0] > .650 + .010
assert 2.7-.1 > 2.5
assert 4.6/(100*rmin_factor*chold_min) < 1/(15e-6)
print('SYS-006/007 arithmetic PASS; declared allocations still require qualification.')
```

Schematic comment for the placed block, with final references substituted:

```text
SYS-007 SOURCE/SHUTDOWN: Vtrip = 0.405*(1+732k/100k) = 3.3696 V.
Corners 3.2637..3.4766 V incl reference, R/TC and SENSE leakage.
EN low -> separate KILL clamp + active rail discharge; no EN/KILL short.
CT=300n C0G; delay must exceed 650ms startup KILL blank (+10ms margin).
Main Ctotal<=1mF: 11.4211ohm max gives 3.6->0.3V in28.38ms.
Held control: 291.6uF min; 1.125mA control +0.817mA reverse =1.942mA.
CE senses held side of output100R; Vinitial=Vtrip_min-max(0.250,Icontrol*102.24).
Combined1uC initial/switching reserve ->46.59ms; low-VIN leakage qualification required.
Restart: raw>=3.70V, held>=3.25V;1s settling PLUS full source qualification.
See ../design/system_power_design.md SYS-007 for full math and qualification.
```

## SYS-008: Autonomous charger and cell-contact temperature circuit

Select **BQ25616RTWR, not BQ25616J**, using the
[TI SLUSDF7A datasheet](https://www.ti.com/lit/ds/symlink/bq25616.pdf),
sections 7, 8.3, 8.5, 9.3 and 10.2, and
[BQ25616EVM SLUUC73A](https://www.ti.com/lit/ug/sluuc73a/sluuc73a.pdf).
The non-J variant has the narrower standard charging-temperature profile.
Use resistor-defined charging, not an assumed power-on I2C configuration.
The alternative BQ25638 resets to 2 A charge, 4.2 V, 3.2 A input and a
60 C hot threshold; those defaults do not satisfy this pack/USB design
without additional always-powered control. Its larger headline current
does not make those defaults safe.

### Native connection basis

| BQ25616 pin | Connection and fitted value |
| --- | --- |
| 1 VAC, 24 VBUS | Tie together at CHARGER_VBUS, after SYS-009 current switch; 1 uF local ceramic |
| 2 ACDRV | Explicit NC; no external ACFET in this 5 V charger block |
| 3 D+, 4 D- | Explicit NC, both floating; do not ground and do not connect the USB-HS pair |
| 5 STAT | CHARGE_STAT_N; optional host read via 10k pullup to +3V3_MCU |
| 6 OTG | GND; sink-only, no battery-to-USB boost operation |
| 7 PG | CHARGER_PG_N; optional host read via 10k pullup to +3V3_MCU |
| 8 ILIM | 453 ohm, 0.1%, 25 ppm/C to GND |
| 9 CE | GND in the qualified fixed-pack assembly; autonomous charge enable |
| 10 ICHG | 604 ohm, 0.1%, 25 ppm/C to GND |
| 11 TS | 68.1k, 0.1%, 25 ppm/C from REGN; 104JT-025 sensor to GND; 10 nF C0G to GND |
| 12 VSET | 10k to GND selects 4.10 V; never leave open (4.2 V) or short (4.35 V) |
| 13, 14 BAT | Protected pack positive; local 2 x 22 uF ceramic; pack negative to GND |
| 15, 16 SYS | SYS_AON; local 2 x 22 uF ceramic, independent of main switched bulk |
| 17, 18, exposed pad | GND |
| 19, 20 SW | 1 uH inductor to SYS_AON |
| 21 BTST | 47 nF ceramic directly to SW, not to GND |
| 22 REGN | 4.7 uF nominal ceramic to GND, per reference circuit; only local charger/TS loads |
| 23 PMID | Local 2 x 22 uF ceramic to GND |

TI confirms that floating both data-detection pins makes the unknown-adapter
case use the ILIM resistor; see the
[manufacturer's unused-DP/DM answer](https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1329976/bq25616-unused-dp-and-dm-pins-setting).
The upstream hardware limiter, not BC1.2 detection, controls USB entitlement.
STAT/PG are status only and do not authorize extra input current. Their
switched pullups must not be changed to REGN or raw USB for convenience.

The 4.10 V profile is specified as 4.0836..4.1164 V for charger junction
temperature 0..85 C. Thus its upper published value remains 83.6 mV below
the pack's 4.2 V charge specification. Thermal regulation at a higher
junction temperature is not a substitute for this 85 C accuracy condition.
Qualify simultaneous audio/charging and restrict charging if that condition
cannot be maintained. Reduced voltage sacrifices some usable capacity;
no percentage or 6 Ah runtime claim is assigned without a pack test.

Using independent resistor tolerance and 100 C temperature excursion:

```text
Rfactor = (1 +/- 0.001) * (1 +/- 25ppm*100C)
Icharge = KICHG / 604ohm, KICHG = 639..677typ..715 A*ohm
Icharge corners = 1.054254 .. 1.187930 A; nominal 1.120861 A
Iinput_charger = KILIM / 453ohm, KILIM = 459..478typ..500 A*ohm
Iinput corners = 1.009709 .. 1.107627 A; nominal 1.055188 A
```

This keeps normal fast-source input demand below SYS-009's minimum higher
limit. In configured-500 mA mode the upstream limiter necessarily controls
input instead; BQ25616's programmable ILIM range starts at 500 mA. Its
VINDPM loop must be qualified with the series limiter for stability and
input-switch dissipation. Do not fit the TPS2553 **-1 latch-off** version:
current limiting during charge is expected, not permission to latch the
USB input off after its overcurrent deglitch time.

The BQ25616 recommended continuous BAT discharge rating is 6 A, versus
the 4 A whole-path allocation in SYS-006. The SW rating is 3.2 A including
both system current and charge current. BATFET maximum resistance is
26 milliohm through 85 C, 30 milliohm through 125 C; its 4 A conduction
screen is 0.416/0.480 W, respectively. These are die-loss calculations,
not a junction-temperature prediction or permission for unlimited system
load. USB charging reduces automatically when the system uses input power;
battery supplement supplies the remainder only when the pack permits it.

Precharge and termination nominally use 5% of the programmed current;
the table's detailed accuracy tests use other RICHG/VBAT points, so do not
invent exact 604-ohm termination limits. A deeply discharged/PCM-open
pack must follow Jauch's approved recovery procedure. The internal safety
timer is nominally 2 h below the low-battery threshold and 10 h fast charge;
the latter has an 8..12 h specification. Input/voltage/thermal regulation
halves its counting rate; TS faults suspend it. A small USB source can
therefore time out before this 6 Ah pack becomes full. Report the fault;
do not automatically cycle CE indefinitely to defeat the safety timer.

### TS resistor derivation and thermal boundary

Use the stocked **Semitec 104JT-025**, 100k at 25 C, +/-1%,
B25/85 4390 K +/-1%, with the
[manufacturer JT resistance table and reliability limits](https://www.semitec-global.com/uploads/2022/01/P9-JT-Thermistor.pdf).
The 025 is the 25 mm lead variant. The longer 050 listing was out of stock
on direct recheck. This thin-film insulated sensor still needs an approved
cell-surface attachment and strain relief; do not crush it between hard
parts, open the pack, or treat a PCB-mounted ambient sensor as cell contact.

No parallel shaping resistor is fitted. For 68.1k from REGN to TS:

```text
f(T) = VTS/VREGN = RNTC(T)/(68100 + RNTC(T))
R_at_threshold = 68100 * f/(1-f)
nominal cold suspend f=0.733 -> 12.514790 C
nominal hot suspend  f=0.4475 -> 37.856876 C
```

Nominal temperatures use log interpolation of Semitec's published R/T table,
not a constant B25/85 extrapolation pretending to be the full curve. The
corner model adds initial +/-1% R and B, one additional +/-1% R/B reliability
shift, top-resistor tolerance/TC, and a **qualification allocation** of
TS leakage within +/-100 nA and REGN at least 3.0 V while charging.
The B delta only models tolerance around the published curve. TI does not
publish a TS-input leakage maximum establishing that allocation.

| Sensor event | Calculated lower C | Calculated upper C |
| --- | ---: | ---: |
| Cold suspend | 10.670935 | 14.245697 |
| Cold resume | 11.918304 | 15.565469 |
| Hot suspend | 36.560694 | 39.200006 |
| Hot resume | 35.431470 | 38.024519 |

These use the **non-J** threshold extrema: cold suspend 72.4..74.2%, cold
resume 71..73%, hot suspend 44.25..45.25%, hot resume 45.55..46.55% of
REGN. A maximum absolute sensor-to-cell temperature error of **5 C**, including
gradient, attachment lag and self-heating, would keep actual charge within
5.670935..44.200006 C. This fits only the cited 2024 pack's 0..45 C window.
The 5 C error is an assembly qualification requirement, not a sensor
datasheet guarantee. Nominal dissipation-constant data are not enough to
approve charging near a hot audio stage. Sensor open drives TS high/cold;
sensor short drives TS low/hot. Both must inhibit charging with the main
MCU physically unpowered. Check harness partial faults and sensor detachment.

### Switching passives and qualification

Use **Vishay IHLP2020BZEK1R0M11**, 1 uH, +/-20%, as the inductor candidate.
This retains the EVM's nominal inductance, with the
[manufacturer's current/inductance and heating curves](https://www.vishay.com/docs/34261/ihlp-2020bz-11.pdf)
and stronger current screen than the small EVM part. Its 7 A saturation
and 7.5 A heating-current figures are typical 25 C characterizations, not
guaranteed hot-current ratings. With an additional allocated 20% DC-bias
inductance loss, Lmin_model=0.64 uH; at 5.5 V and minimum switching
frequency 1.32 MHz, D=0.5 gives 1.627604 A ripple peak-to-peak and
4.013802 A peak at the full 3.2 A SW screen. Actual operating points with
the approximately 1.1 A input budget are lower; nevertheless verify hot
inductance, winding/core loss and short-circuit behavior.

For BAT, SYS and PMID, start with two **GRM32ER71C226KEA8L** 22 uF,
16 V, +/-10%, X7R capacitors per node. The combined effective local
capacitance must exceed 10 uF at working voltage, temperature and age;
nominal 44 uF alone is not proof. A screening retention allocation of
50% DC bias, 15% temperature, 10% initial tolerance and 3% aging yields
16.3251 uF. The 50% is a release criterion to verify with Murata SimSurfing
and installed measurements, **not a curve value obtained in this review**.
Use the stocked EVM **GRM155R71E473KA88D** for the 47 nF bootstrap.
For REGN, nominate **GCM21BR71C475KA73L**, 4.7 uF, 16 V, +/-10%, X7R,
0805, replacing the poorly stocked 0603 X5R EVM GRM188R61C475KAAJD.
[Murata's part sheet, distributor-hosted](https://www.farnell.com/datasheets/2079124.pdf)
establishes its nominal specification, not a minimum under DC bias.
The same stocked 4.7 uF candidate serves the USB-control LDO output;
its effective capacitance there must remain >=1 uF. Confirm REGN startup,
loop stability and effective capacitor behavior before passive approval.
For the 1 uF bypass candidates, reuse the already sourced TDK
C2012X7R1E105K125AB from BTN-004 rather than adding an unexplained new MPN;
its installed effective value and the combined USB inrush still need review.

Required before charge-enabled release: approved pack revision and harness;
thermal sensor attachment/error proof; TS leakage/REGN bounds; charger
junction <=85 C during charge; capacitor bias/age evidence; hot inductor
proof; input-limiter/VINDPM transient stability; battery removal/PCM-open
behavior; and SYS_AON overshoot within every downstream recommended limit.
Upstream VBUS/CC ESD and overvoltage protection remain a separate connector
protection closure item; the charger's 14.2 V typical OVP is not protection
for the 6.5 V input switch or the CC interface.

## SYS-009: USB-C budget, configured-500 mA latch and input switch

This is a sink-only, 5 V circuit. It supports charging while the reader is
off from a Type-C source advertising at least 1.5 A, and charging from an
ordinary USB 2.0 host only after a fresh configured-500 mA permission.
Default/unconfigured and default-current suspend states leave the charger
input **off**. The reader and USB data engine run from the approved battery
in those states. A depleted/absent battery is not promised USB-DAC startup
or full-output audio from a legacy host. No 9 V PD mode is added.

### Current-state truth table

| TUSB OUT1 | OUT2 | Source state | Permitted charger-input behavior |
| --- | --- | --- | --- |
| High | High | Unattached | Off; erase configured permission |
| High | Low | Default USB current | Off unless configured-500 latch set; then lower hardware limit |
| Low | High | 1.5 A advertised | Higher hardware limit, regardless of application on/off |
| Low | Low | 3 A advertised | Same higher hardware limit; this revision deliberately does not use all 3 A |

This is [TUSB320LAI Table 3](https://www.ti.com/lit/ds/symlink/tusb320lai.pdf),
not a guessed binary current code. Set PORT to GND (UFP), ADDR open (GPIO),
and EN_N to GND. The CC controller provides the Rd terminations, including
dead-battery behavior; do not add parallel 5.1k resistors. CC1 and CC2 remain
separate connector signals. VBUS_DET uses **887k**, +/-1%, 100 ppm/C series
resistance from raw connector VBUS; its 869.349..904.829k corner fits the
855..920k requirement. OUT3 and ID are unused explicit NCs.

Power CC/USB permission logic from **+3V3_USB_CTRL**, not +3V3_MCU or
AON_HOLD. TUSB's recommended VDD maximum is 5.0 V; raw 5 V USB may exceed
that. Use **TPS7B8133DRVR** from connector VBUS: pin 1 IN and pin 2 EN to
VBUS, pins 3/4 and exposed pad GND, pin 5 DNC tied GND, pin 6 OUT to
+3V3_USB_CTRL. Fit 1 uF input and 4.7 uF output ceramic, with at least
1 uF effective output capacitance per the
[TPS7B81 specification](https://www.ti.com/lit/ds/symlink/tps7b81.pdf).
Fit **3.09k**, +/-1%, 100 ppm/C from output to GND: its 1.031214 mA
minimum preload satisfies the regulator's >=1 mA output-accuracy test
condition. This is a USB-only load, not a permanent battery drain.
The selected regulator's input rating does not protect other USB parts
against overvoltage. Qualify the TUSB-required <=25 ms VDD ramp.

### Input-current switch

Select **TPS2553DRVR**, the non-latching WSON version, using
[SLVS841F](https://www.ti.com/lit/ds/symlink/tps2553.pdf). Its pinout is
not the SOT23 pinout: **IN 6, GND 5, EN 4, FAULT 3, ILIM 2, OUT 1**, with
exposed pad GND. IN connects to protected 5 V connector VBUS and 1 uF
local ceramic; OUT is CHARGER_VBUS. FAULT is optional status, otherwise NC.

ILIM has **61.9k**, 0.1%, 25 ppm/C permanently to GND. A second **30.1k**,
same grade, connects from ILIM to TMUX1102 pin 1 D; TMUX pin 2 S is GND.
TMUX pin 3 GND, pin 4 SEL = raw TUSB OUT1. **TMUX pin 5 VDD and its
100 nF bypass share the exact protected VBUS node with TPS2553 IN 6.**
There is no intervening diode, filter or load switch between their supply
pins that could cause different decay. Do not power the TMUX from the
delayed +3V3_USB_CTRL island: its fail-safe feature protects SEL, not S/D.
**TMUX1102 is active low**; do not substitute TMUX1101 without changing
polarity. Thus a qualified >=1.5 A advertisement adds the parallel resistor.
Its [full-temperature switch table](https://www.ti.com/lit/ds/symlink/tmux1102.pdf)
gives 4.9 ohm maximum on resistance at VDD=4.5..5.5 V and
0.9 nA off / 2 nA on leakage maxima. A 2 mA additional current-limit
allocation below comfortably exceeds this analog error; it is not a
license to replace it with an unqualified leaky MOSFET.

The same table's SEL thresholds are VIH=1.49 V and VIL=0.87 V; the
3.3 V TUSB pullup therefore remains appropriate. SEL tolerates up to
5.5 V independently of TMUX supply state. ILIM has no external source or
added storage capacitor; its only source is the TPS2553's internal circuit,
now powered from the same node as the mux. This removes the independent
powered-ILIM/unpowered-mux supply path without assuming that disabled
TPS2553 makes ILIM zero. Independent review accepted this topology fix.
Keep the shared node <=5.5 V, including protection overshoot, and verify
ILIM-to-VDD transient ordering on hotplug/removal. Below 4.5 V the
maximum-current safety screen still uses nonnegative RON and RON=0 as
the worst case, but lower-current availability is not guaranteed by the
5 V on-resistance table. Do not interpolate a new guarantee between tables.

For R in kilohms, TI's programming equations give current in mA:

```text
Imin=25230/R^1.016; Inom=23950/R^0.977; Imax=22980/R^0.94
Lower state R=61.9k:       380.204 .. 425.426typ .. 477.080 mA
Higher R=61.9k||30.1k:   1183.046 ..1267.316typ ..1363.638 mA
```

The above includes resistor tolerance/TC. At 4.5..5.5 V, maximum switch
resistance changes the higher-state minimum to 1182.914934 mA; the final
verification block includes it.
Allocate 2 mA for analog/programming deviations plus **2 mA total USB-control
load**: upper aggregate screens remain 481.080 mA <500 mA and
1367.638 mA <1.5 A. The 2 mA control ceiling is a measured system requirement;
TUSB quiescent current and some regulator conditions are typical-only, so
this is not a claimed sum of guaranteed maxima. Require total suspended
VBUS draw <=2.5 mA with charger input off, including the USB data interface,
pullups, protection leakage and preload. Do not omit preload from that test.

The selected charger input maximum 1.107627 A is below the higher switch
minimum, so continuous ordinary charging should not force this switch into
linear current limit. In the lower state it will; at 5.5 V source and 4.3 V
charger input, the upper 0.479080 A screen dissipates 0.574896 W in the
switch. Check the actual thermal path and coupled VINDPM response. At
normal 1.107627 A and 150 milliohm RON, conduction loss is about 0.184 W.
The DRV package's board-dependent thermal impedance is not a heat guarantee.

### Permission logic and exact connections

All logic below uses +3V3_USB_CTRL, each with its own 100 nF bypass.
TUSB OUT1/OUT2 each have 47k pullup to **that same rail**; neither is pulled
to the MCU rail. The only MCU-driven inputs are the fail-safe clock input
and insulated NMOS gates. Two logical firmware ports are reserved here;
final nonconflicting RA8P1 pad assignments belong in the native controller
pin-allocation table.

| Device / pin | Connection |
| --- | --- |
| TPS3839G33DBZR 1 GND, 3 VDD | GND; +3V3_USB_CTRL |
| TPS3839 2 RESET | USB_CTRL_RESET_N; **push-pull**, no direct wired-OR connection |
| USB_CTRL_RESET_N to EN | 39k series to TPS2553 EN; 56k EN to GND |
| USB_CTRL_RESET_N to clear | 10k series to USB_ENUM_CLR_WIRE; this resistor also supplies its pullup |
| SN74LVC3G17DCUR 1/7 (1A/1Y) | TUSB OUT1 / USB_CC_OUT1_B |
| SN74LVC3G17 3/5 (2A/2Y) | TUSB OUT2 / USB_CC_OUT2_B |
| SN74LVC3G17 6/2 (3A/3Y) | USB_ENUM_CLR_WIRE / USB_ENUM_CLR_N |
| SN74LVC3G17 4/8 | GND / +3V3_USB_CTRL |
| SN74LVC1G74DCUR 1 CLK | USB_ENUM_SET_PULSE, 47k pulldown to GND; MCU initially drives low |
| SN74LVC1G74 2 D, 7 PRE, 8 VCC | All +3V3_USB_CTRL |
| SN74LVC1G74 3 inverted Q | USB_ENUM_NOT_OK |
| SN74LVC1G74 4 GND, 5 true Q | GND; unused true Q explicit NC |
| SN74LVC1G74 6 CLR | USB_ENUM_CLR_N from the Schmitt buffer |
| Nexperia 74LVC2G38DP,125 1/2/7 | 1A=USB_CC_OUT1_B, 1B=USB_CC_OUT2_B, OD 1Y=USB_ENUM_CLR_WIRE |
| 74LVC2G38 5/6/3 | 2A=USB_CC_OUT1_B, 2B=USB_ENUM_NOT_OK, OD 2Y=TPS2553 EN |
| 74LVC2G38 4/8 | GND / +3V3_USB_CTRL |
| DMN2056U-7 clear FET | S2=GND, D3=USB_ENUM_CLR_WIRE, G1=USB_PERMISSION_CLEAR_H via 100 ohm; 47k gate pulldown |
| DMN2056U-7 power-off FET | S2=GND, D3=USB_ENUM_CLR_WIRE, G1=POWER_OFF_H via 100 ohm; **1M** gate pulldown |

The 1M on the held-domain POWER_OFF_H FET avoids adding a 47k static load
to the SYS-007 hold reservoir. Include its gate charge and <=4.6 uA pull
current in that section's existing allocations. No gate or drain connects
USB-control supply directly to the MCU reset or kill domain.

Primary logic pin and timing sources:
[SN74LVC3G17](https://www.ti.com/lit/ds/symlink/sn74lvc3g17.pdf),
[SN74LVC1G74](https://www.ti.com/lit/ds/symlink/sn74lvc1g74.pdf),
[Nexperia 74LVC2G38](https://assets.nexperia.com/documents/data-sheet/74LVC2G38.pdf),
and [TPS3839](https://www.ti.com/lit/ds/symlink/tps3839.pdf).
The triple true-Schmitt buffer is deliberate: ordinary LVC gates and the
flip-flop still specify 10 ns/V input-transition limits even when marketing
mentions Schmitt action. Slow 47k open-drain edges and the wired clear
must not drive those inputs directly. Do not delete this buffer as redundant.

TPS3839G33's falling threshold is 3.003..3.126 V and release delay
120..350 ms. Its nominal hysteresis is not a bounded upper release
guarantee; verify release on the selected regulator's actual output range.
With 39k/56k +/-1%, 100 ppm/C and 3 uA adverse EN-node leakage allocation:

```text
VUSB_CTRL_min = 3.3*(1-.015) = 3.2505 V (with preload)
VRESET_high_min = VUSB_CTRL_min - .4 = 2.8505 V
VEN_high_min = 1.583411 V > TPS2553 threshold upper 1.1 V
EN clamp current max = 90.628421 uA < gate 100uA VOL test
Gate VOL <= .1 V; TPS2553 turn-off threshold minimum .66 V
Total supervisor source screen = 434.379263 uA < .5mA VOH test
Unknown RESET<=.9 V startup screen: VEN<=.607908 V <.66 V
```

The last line is a bounded startup screen, not a replacement for cold-ramp
measurements: validate the 3 uA leakage allocation during sub-rated logic
supply, input/output capacitor discharge ordering, and no transient EN
assertion. At VDD>=0.9 V the supervisor specifies VOL<=0.4 V. At operating
voltage the clear low must meet the Schmitt falling threshold with all
clamp/supervisor leakage; its output must clear the flip-flop before EN
can be released. Published thresholds at individual VCC test points do not
justify inventing an interpolation guarantee. Qualify complete ramp sweeps.

### USB event contract and usable power

An application port generates a **fresh positive pulse** only after the
USB stack has a currently configured 500 mA budget; a remembered static
GPIO level is not used as permission. Another port asserts
USB_PERMISSION_CLEAR_H for suspend, bus reset, deconfiguration and initial
firmware startup. Hold clear during those states; return the clock low
before release and permit a new pulse only after fresh authorization.
Detach clears through the CC outputs and loss of the USB control rail;
main power-off clears through POWER_OFF_H. The hardware latch enforces
default/reset behavior but does not decode USB packets autonomously:
the USB event-handling contract is required for USB-current compliance.
MCU reset does not directly clamp the latch; clearing on firmware startup
is a software obligation, not an autonomous MCU-reset clearing claim.
Battery charging voltage/current/temperature safety remains autonomous.

For a valid >=1.5 A Type-C advertisement, charge remains allowed during
application off/suspend, independent of the configured-500 latch. A reduced
advertisement removes that higher-current setting; an unconfigured default
source then leaves the charger off. Verify rapid detach/reattach, held-up
USB-control caps, advertisement transitions and default USB bus reset;
do not infer a zero-latency detach response from a static truth table.

At 5 V and nominal charger input 1.055188 A, 90% assumed conversion
efficiency yields **4.748344 W** shared by system and charging. The existing
host plus baseline headphone screen alone is 6.179018 W before other
loads: it requires battery supplement even on this Type-C input. At the
lower input limit, available system power is much smaller. High-quality
audio remains in scope, but full-load audio plus net charging is not a
promise from a 500 mA host or this intentionally conservative 5 V input.
These are continuous engineering screens; music-average demand can be
lower and is not a substitute for transient/thermal limits.

### SYS-008/009 sourcing snapshot

Observed 2026-09-07, USD quantity-one cut tape unless stated. Stock/pricing
are snapshots, not reserved inventory. No purchase has been made. Recheck
before ordering and keep exact suffixes in the symbol fields/BOM.

| Qty | Part / role | Source, available quantity, unit cost |
| ---: | --- | --- |
| 1 | BQ25616RTWR autonomous charger | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/BQ25616RTWR/11615973), 1980, $2.82; qty10 $2.101 |
| 1 | TUSB320LAIRWBR CC controller | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TUSB320LAIRWBR/5722618), 16142, $1.83 |
| 1 | TPS2553DRVR non-latching limiter | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TPS2553DRVR/2047902), 12935, $1.06 |
| 1 | TPS7B8133DRVR USB-control LDO | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TPS7B8133DRVR/13176915), 10116, $1.36 |
| 1 | TPS3839G33DBZR USB reset | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TPS3839G33DBZR/3748985), 3871, $0.85 |
| 1 | SN74LVC1G74DCUR permission latch | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC1G74DCUR/2195376), 52638, $0.60 |
| 1 | SN74LVC3G17DCUR true-Schmitt edge conditioning | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC3G17DCUR/863652), 33917, $0.41 indexed |
| 1 | Nexperia 74LVC2G38DP,125 open-drain NAND | [DigiKey](https://www.digikey.com/en/products/detail/nexperia-usa-inc/74LVC2G38DP-125/1231599), 3108, $0.35 |
| 1 | TMUX1102DBVR active-low resistor switch | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TMUX1102DBVR/11308856), 6145, $2.21 |
| 1 | Semitec 104JT-025 cell-contact sensor | [DigiKey](https://www.digikey.com/en/products/detail/semitec-usa-corp/104JT-025/16578934), 1836, $0.70 |
| 2 | DMN2056U-7 clear clamps | Reuse the SYS-007 sourced transistor; additional quantity two |
| 1 | IHLP2020BZEK1R0M11 inductor candidate | [DigiKey stock listing](https://www.digikey.com/en/products/filter/fixed-inductors/71?s=N4IgjCBcoLQCxVAYygMwIYBsDOBTANCAG4B2aWehA9lANogDMATAJwMDsIAuoQA4AuUECAC%2BYoA), 2418, $1.01 indexed; thermal/bias proof pending |
| 6 | GRM32ER71C226KEA8L local bulk candidates | [DigiKey](https://www.digikey.com/en/products/detail/murata-electronics/GRM32ER71C226KEA8L/3465249), 30854, $0.88; qty10 $0.552; bias/aging proof pending |
| 2 | GCM21BR71C475KA73L REGN / USB-control output candidates | [DigiKey 490-5332-1-ND](https://www.digikey.com/en/products/detail/murata-electronics/GCM21BR71C475KA73L/2075312), 262419, $0.29; bias/stability proof pending |
| 1 | GRM155R71E473KA88D bootstrap, 47 nF | [DigiKey 490-3254-1-ND](https://www.digikey.com/en/products/detail/murata-electronics/GRM155R71E473KA88D/702795), 674464, $0.10 |
| 1 | RT0603BRD07604RL ICHG, 604 ohm | [DigiKey YAG1701CT-ND](https://www.digikey.com/en/products/detail/yageo/RT0603BRD07604RL/1072600), 3874, $0.10 |
| 1 | RT0603BRD07453RL ILIM, 453 ohm | [DigiKey YAG4559CT-ND](https://www.digikey.com/en/products/detail/yageo/RT0603BRD07453RL/6616715), 17973, $0.10 |
| 1 | RG1608P-6812-B-T5 TS top, 68.1k | [DigiKey RG16P68.1KBCT-ND](https://www.digikey.com/en/products/detail/susumu/RG1608P-6812-B-T5/1241006), 3942, $0.12 |
| 1 | RT0603BRD0761K9L USB lower limit, 61.9k | [DigiKey YAG1704CT-ND](https://www.digikey.com/en/products/detail/yageo/RT0603BRD0761K9L/5139152), 3617, $0.10 |
| 1 | RT0603BRD0730K1L USB higher-limit branch, 30.1k | [DigiKey YAG1631CT-ND](https://www.digikey.com/en/products/detail/yageo/RT0603BRD0730K1L/5139079), 780, $0.11 |

The five sourced precision resistors above retain the calculated 0.1%,
25 ppm/C grade. The originally proposed RT0603BRD0768K1L 68.1k row is
currently out of stock on direct recheck; replace it with the stated Susumu
RG1608P-6812-B-T5. Its P temperature code and B tolerance are established
by the [manufacturer RG series specification](https://www.susumu.co.jp/dl/?filename=n_catalog_partition01_en.pdf&type=application/pdf).
The generic 1% control/bypass passives remain source-work items, not hidden
zero-cost lines. Stock/lifecycle verification is not a substitute for the
electrical and assembly qualification gates. Do not use
the out-of-stock TPS2553DBVR, SN74LVC1G74DCTR, TPS70933DBVR or obsolete
GRM31CR61E226KE15L merely because a cached listing showed earlier stock.

### Reproducible charger, TS and USB arithmetic

Run this block with Python 3; it does not generate or change KiCad design.
Assertions verify stated arithmetic and logic truth tables, not the
datasheet limits or physical qualification allocations themselves.

```python
from itertools import product
from math import exp, isclose, log

precision_lo, precision_hi = .999*.9975, 1.001*1.0025
control_factors = (.99*.99, 1.01*1.01)
icharge_lo = 639/(604*precision_hi)
icharge_hi = 715/(604*precision_lo)
iinput_lo = 459/(453*precision_hi)
iinput_hi = 500/(453*precision_lo)
rfast_nom_k = 1/(1/61.9+1/30.1)
rfast_lo_k = rfast_nom_k*precision_lo
rfast_hi_k = 1/(1/(61.9*precision_hi)+1/(30.1*precision_hi+.0049))
ilow_hi_ma = 22980/(61.9*precision_lo)**.94
ifast_hi_ma = 22980/rfast_lo_k**.94
ifast_lo_ma = 25230/rfast_hi_k**1.016
vctrl_min, vctrl_max = 3.3*.985, 3.3*1.015
en_hi = min(
    ((vctrl_min-.4)/(39e3*a)-3e-6)/(1/(39e3*a)+1/(56e3*b))
    for a, b in product(control_factors, repeat=2)
)
en_start = max(
    (.9/(39e3*a)+3e-6)/(1/(39e3*a)+1/(56e3*b))
    for a, b in product(control_factors, repeat=2)
)
en_sink = vctrl_max/(39e3*control_factors[0])+3e-6
por_source = (vctrl_max/(39e3*control_factors[0])
              + vctrl_max/(10e3*control_factors[0])+5e-6)
ripple = 5.5*.25/(1.32e6*1e-6*.8*.8)
checks = {
    'charge nominal A': (677/604, 1.1208609271523178),
    'charge lower A': (icharge_lo, 1.0542544935040519),
    'charge upper A': (icharge_hi, 1.1879296182770098),
    'charger input nominal A': (478/453, 1.055187637969095),
    'charger input lower A': (iinput_lo, 1.0097085289897962),
    'charger input upper A': (iinput_hi, 1.1076266837081676),
    'configured input lower mA': (25230/(61.9*precision_hi)**1.016, 380.2041010094958),
    'configured input nominal mA': (23950/61.9**.977, 425.42573089094265),
    'configured input upper mA': (ilow_hi_ma, 477.0799062546294),
    'Type-C input lower with switch at 4.5..5.5V mA': (ifast_lo_ma, 1182.9149338675961),
    'Type-C input nominal mA': (23950/rfast_nom_k**.977, 1267.3162242885946),
    'Type-C input upper mA': (ifast_hi_ma, 1363.6382575960565),
    'EN high minimum V': (en_hi, 1.5834108465184664),
    'EN startup allocation V': (en_start, .607908061033566),
    'EN clamp sink A': (en_sink, .00009062842096175428),
    'USB POR source A': (por_source, .00043437926271259596),
    'LDO preload minimum A': (vctrl_min/(3090*control_factors[1]), .0010312143393518434),
    'inductor ripple A pp': (ripple, 1.627604166666667),
    'inductor peak screen A': (3.2+ripple/2, 4.013802083333333),
    'bulk effective model F': (44e-6*.9*.85*.97*.5, .0000163251),
    'Type-C available SYS power W': (5*(478/453)*.9, 4.748344370860927),
    'normal switch conduction W': (iinput_hi**2*.150, .18402553056935297),
    'low-limit switch linear screen W': ((5.5-4.3)*(ilow_hi_ma/1000+.002), .5748958875055554),
}
for name, (actual, expected) in checks.items():
    assert isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-12), (name, actual, expected)
    print(f'{name}: {actual:.12g}')
assert icharge_hi < 1.2
assert iinput_hi*1000 < ifast_lo_ma
assert ilow_hi_ma+2+2 < 500
assert ifast_hi_ma+2+2 < 1500
assert en_hi > 1.1 and en_start < .66
assert en_sink < 100e-6 and por_source < .5e-3

# H/H unattach clears the latch. OD EN clamp = OUT1_B AND NOT_Q.
for out1, out2, permitted in product((False, True), repeat=3):
    unattached = out1 and out2
    q_effective = permitted and not unattached
    clamp_en = out1 and not q_effective
    allowed = (not out1) or q_effective
    assert allowed == (not clamp_en)
    if unattached:
        assert not allowed

rt_table = ((0, 365e3), (10, 212.5e3), (20, 127.7e3),
            (25, 100e3), (30, 78.88e3), (40, 50.03e3), (50, 32.51e3))
def r_nominal(t):
    for (t0, r0), (t1, r1) in zip(rt_table, rt_table[1:]):
        if t0 <= t <= t1:
            return exp(log(r0)+(t-t0)/(t1-t0)*log(r1/r0))
    raise ValueError(t)

def ts_temperature(threshold, r_top, r_factor, b_delta, leakage):
    lo, hi = 0., 50.
    for _ in range(80):
        t = (lo+hi)/2
        rn = r_nominal(t)*r_factor*exp(
            b_delta*4390*(1/(t+273.15)-1/298.15))
        f = rn/(r_top+rn)+leakage*(r_top*rn/(r_top+rn))/3.0
        if f > threshold:
            lo = t
        else:
            hi = t
    return (lo+hi)/2

ts_cases = (
    ('cold suspend', (.724, .742), (10.670935023135442, 14.245696807728272)),
    ('cold resume', (.71, .73), (11.918304140794191, 15.565468655101228)),
    ('hot suspend', (.4425, .4525), (36.56069376252326, 39.200006469568294)),
    ('hot resume', (.4555, .4655), (35.431470320094306, 38.02451931873179)),
)
results = {}
for name, thresholds, expected in ts_cases:
    temperatures = [ts_temperature(th, 68100*rr, rf, bd, il)
        for th, rr, rf, bd, il in product(
            thresholds, (precision_lo, precision_hi),
            (.99*.99, 1.01*1.01), (-.0201, .0201), (-100e-9, 100e-9))]
    actual = min(temperatures), max(temperatures)
    assert all(isclose(a, b, abs_tol=1e-8) for a, b in zip(actual, expected))
    results[name] = actual
    print(name, actual)
assert results['cold suspend'][0]-5 > 0
assert results['hot suspend'][1]+5 < 45
print('SYS-008/009 arithmetic and static truth tables PASS; qualification gates remain.')
```

Concise linked schematic comments, with actual references substituted:

```text
SYS-008 CHARGE: BQ25616 non-J, VSET 10k -> 4.10 V (max 4.1164 V, TJ 0..85 C).
ICHG 604R: 677/604 = 1.120861 A; worst 715/(604*.999*.9975) = 1.187930 A.
TS: 68.1k to REGN + 104JT-025 100k to GND; open/short inhibits charge.
Nominal cold/hot 12.515/37.857 C. Model corners 10.671..14.246 / 36.561..39.200 C.
Requires approved 2024 pack and <=5 C sensor-cell error; see SYS-008 full proof.
```

```text
SYS-009 USB: default/unconfigured or default-current suspend -> charger OFF.
Fresh configured 500 mA latch: 61.9k -> 380.204..477.080 mA + 4 mA allocations.
CC >=1.5 A: add 30.1k -> 1182.915..1363.638 mA at 4.5..5.5 V input.
TMUX VDD shares TPS2553 IN exactly; SEL stays 3.3 V. Both CC levels use same limit.
Detach/off/reset/suspend clear permission; do not bypass triple Schmitt buffer.
Input cannot sustain full audio + charge; see SYS-009 power/event/qualification proof.
```
