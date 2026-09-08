# Proposed TPS63806 main-rail brownout and reset contract

Revision 1, 2026-09-08. **PROPOSED, NOT ADOPTED.** This is a preserved
engineering fallback, not a native implementation report, fabrication release,
or claim of electrical qualification. Tracking: [#846](https://github.com/bsikar/ra8-firmware/issues/846),
[#825](https://github.com/bsikar/ra8-firmware/issues/825), and
[epic #821](https://github.com/bsikar/ra8-firmware/issues/821).

The input contract is [PWR-006](main_regulator_tps63806.md): main load-pin
voltage 3.151819680..3.393012496 V, including its single combined 75 mV
disturbance allocation. The proposed radio path loses at most 20 mV at its
complete load. Neither that switch selection nor this reset circuit is adopted
by this document. New component references remain unassigned.

## Existing references and scope

- [RST-001](boot_and_debug.md#rst-001-external-supervisor-implementation-basis):
  U2 TPS3808G33DBVR, C44 bypass, CT open, R1 reset pull-up, MCU_RESET_N,
  and TP1/SW_RESET_N manual reset.
- [RADIO-011 through RADIO-015](radio_interface.md#radio-011-radio-supervisor-threshold-design):
  U6 TPS389001DSET, C51 bypass, C52 release timing, R11/C6_EN, R12/R13
  sense divider, and U7 SN74LVC1G97DBVR manual-reset arbitration.
- [PWR-004 held controls](main_regulator_ltc3119.md#raw-run-buffer-and-held-shutdown-clamp):
  U16 raw SYS_AON buffer, R68/R69 enable network, Q3/R70/R71 held clamp,
  R34 110 kOhm, U12, KILL and main discharge remain unchanged.

The standard RA8P1 has a general VCC/VCC2 operating range of 1.62..3.63 V;
3.0 V is not an intrinsic universal CPU minimum. However, Table 2.57's SDRAM
controller timing conditions require VCC/VCC_DCDC/VBATT >=3.0 V. The selected
IS42S32160F-7TLI memory and ESP32-C6 module also have 3.0 V operating minima.
The before-3-V reset policy below protects the system's operating boundary;
asserting RES alone does not prove that every SDRAM transaction has already
ceased. Internal reset-to-bus-quiescence timing and data-integrity policy need
their own decision. This record does not change the VBATT policy in PWR-004.
Sources: [RA8P1 datasheet, operating conditions and Table 2.57](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet),
[ISSI-authored SDRAM datasheet](https://www.farnell.com/datasheets/4555730.pdf),
[Espressif module datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf).

Normal operation must stay inside the regulator/load operating envelope. The
fault screen covers bounded load discharge and controlled power loss, not
an arbitrary short circuit, unlimited fault current, or guaranteed preservation
of RAM through a supply violation. Measurements are qualification work, not
a prerequisite for drawing this explicitly conditional prototype.

## Delayed-release supervisor proposal

| Role | Proposed exact component/change | Minimum falling threshold | Maximum release threshold |
| --- | --- | ---: | ---: |
| U2 | TPS3808G01DBVR; top RT0603BRD0761K9L 61.9k; bottom RT0603BRD0710KL 10k; retain CT open | 2.824599541 V | 3.090430601 V |
| U6 | Retain TPS389001DSET/C52; R12 becomes RT0603BRD0732K4L 32.4k; retain R13 RG1608P-203-B-T5 20k | 2.959633850 V | 3.092186895 V |

These falling thresholds are fallback conditions, NOT the primary before-3-V
protection. U2 G01 has +/-2% threshold accuracy and at most 3% hysteresis;
do not reuse the fixed G33 limits. CT open retains 12..28 ms release delay.
U6 retains +/-1% threshold accuracy, 0.825% maximum hysteresis and RADIO-012's
C52 delay calculation. Neither device specifies a maximum falling propagation
delay. U6 requires an MR pulse at least 1 us long; TPS3808's MR pulse-width
entry is typical, not the same guaranteed 1 us condition.
[TPS3808 Rev N, sections 6.5-6.6](https://www.ti.com/lit/ds/symlink/tps3808.pdf),
[TPS3890, sections 7.5-7.6](https://www.ti.com/lit/ds/symlink/tps3890.pdf).

Each divider resistor has independent 0.1% initial tolerance, 25 ppm/C over a
100 C excursion, and an allocated +/-0.15% combined assembly/aging factor.
SENSE current allocations are +/-75 nA for U2 (25 nA listed plus 50 nA board)
and +/-150 nA for U6 (100 nA listed test-point limit plus 50 nA board).
Those board/lifetime allowances are acceptance conditions, not silicon limits.
Primary part coding: [YAGEO RT specification](https://yageogroup.com/content/datasheet/asset/file/PYU-RT_1-TO-0-01_ROHS_L),
[Susumu RG catalog](https://www.susumu.co.jp/common/pdf/n_catalog_partition01_en.pdf).

## Immediate assertion, separately stretched reset

Use one channel for the actual MCU supply node and one for the actual radio
supply node. All added IC supply pins connect to +3V3_MCU, NOT SYS_AON or
AON_HOLD. Radio-off must not itself assert MCU reset.

### Per-channel fast detector

| Part/function | Proposed connection |
| --- | --- |
| TLV3501AIDBVR | Pin 4 V+ to main; pin 2 V- to GND; pin 6 SHDN to GND; pin 5 OUT is local FAST_GOOD |
| RT0603BRD07226RL 226 ohm | Monitored rail to divider node |
| RG1608P-102-B-T5 1k | Divider node to GND |
| RG1608P-102-B-T5 1k input limiter | Divider node to comparator pin 3 IN+ |
| RG1608P-102-B-T5 1k reference limiter | Shared 2.5 V reference to comparator pin 1 IN- |
| SN74LVC2G07DCKR | Pins 1/3 both from FAST_GOOD; pin 5 main; pin 2 GND; pin 6 clamps protected reset/enable; separate pin 4 clamps supervisor MR |

For the MCU channel, the protected net is MCU_RESET_N and MR is U2.3.
For the radio channel, the protected net is C6_EN and MR is U6.3.
Keep all MR and protected-output drains separate. Do not join MR to RESET.
The input limiters are important when radio storage remains charged after
main power disappears; the conservative input-clamp screen is below 2.49 mA.

TLV3501 specifies 12 ns maximum propagation at 5 mV overdrive across
temperature, with the specified step/load conditions. Its 6.5 mV offset limit
is at 25 C; temperature drift and 6 mV typical hysteresis are not guaranteed
maxima. The qualification allocations below deliberately expose those gaps.
[TLV3501, sections 6.1 and 6.6-6.7](https://www.ti.com/lit/ds/symlink/tlv3501.pdf).

### Per-channel pulse extension

| Part/function | Proposed connection |
| --- | --- |
| SN74LVC1G123DCTR | Pin 1 A from FAST_GOOD; pins 2 B, 3 CLR and 8 VCC to main; pin 4 GND |
| RT0603BRD0710KL 10k | Main to monostable pin 7 Rext/Cext |
| GRM31C5C1H104JA01K 100n C0G | Between monostable pins 7 and 6 Cext; NOT a ground-referenced timing capacitor |
| SN74LVC2G06DCKR | Pins 1/3 both from monostable pin 5 Q; pin 5 main; pin 2 GND; pin 6 clamps protected reset/enable; separate pin 4 clamps MR |

The direct path asserts promptly; the monostable extends a short dip and
restarts the supervisor release delay. Both direct and extended clamps act
on the protected pin, not only MR. The two open-drain pairs prevent feedback
self-latching. Validate pulse continuity where the direct and extended
responses overlap, including repeated dips and power ramps; do not assume
that an arbitrarily narrow comparator pulse satisfies all receiver timing.

At 3.3 V +/-0.3 V, TI specifies a 3 ns minimum trigger pulse and 13.2 ns
maximum trigger-to-Q delay over temperature. Its 100 nF/10k pulse limits
give a 0.941479..1.165431 ms component-allocation screen here. Use the broader
0.90..1.25 ms assembled-circuit acceptance window for timing-node leakage
and process effects. Existing supervisor delays then govern release.
[SN74LVC1G123, sections 5.6 and 5.9](https://www.ti.com/lit/ds/symlink/sn74lvc1g123.pdf).

### Existing radio arbitration and logic levels

U7.4 presently drives RADIO_MR_N push-pull. Insert SN74LVC1G07DBVR between
U7.4 and U6.3 before wiring any open-drain clamp there. Add RC0603FR-074K7L
4.7k from the new MR node to main; use the same external pull-up on U2 MR.
Retain TP1/SW_RESET_N as the independent manual MCU-reset request.

With 100 uA total adverse MR-node current allocated, the 4.7k screen gives
MR high >=2.519834 V at main=3 V and sink current <=0.837682 mA at maximum
main voltage. The allocation includes supervisor MR behavior, added drivers
and board leakage; U6 does not provide a separate guaranteed MR-current
maximum. Do not relabel the allocation as a measured load.

The LVC drivers' 0.4 V maximum low at the 3 V/16 mA test point is below
MCU RES's 0.2*VCC and radio EN's 0.25*VDD low limits at 3 V. Keep complete
protected-net sink load <=1 mA and total node capacitance <=50 pF. The
comparator-to-LVC interface accepts main-rail CMOS levels; allocate comparator
VOL <=0.2 V and VOH >=VCC-0.2 V over the selected temperatures, because its
listed 50 mV output-swing limit is not a full-temperature guarantee.
[SN74LVC2G07](https://www.ti.com/lit/ds/symlink/sn74lvc2g07.pdf),
[SN74LVC2G06](https://www.ti.com/lit/ds/symlink/sn74lvc2g06.pdf),
[SN74LVC1G07](https://www.ti.com/lit/ds/symlink/sn74lvc1g07.pdf).

## Reference, startup and power-domain contract

Use one REF3425IDBVR. Pins 1 GNDF and 2 GNDS join local quiet ground;
pins 3 EN and 4 IN connect to main; pins 5 OUT_S and 6 OUT_F join the
local reference output. Fit C2012X7R1E105K125AB 1 uF there, with effective
capacitance inside TI's 0.1..10 uF stable range. Reuse C1608X7R1H104K080AA
100 nF for each IC supply bypass. These capacitor selections inherit their
existing [power-decoupling](power_decoupling.md) and control-island sourcing
bases, rather than claiming a new procurement check for them.

The reference has +/-0.05% initial accuracy, 6 ppm/C maximum box drift,
15 ppm/V maximum line regulation and 95 uA maximum quiescent current.
Its no-load dropout limit is 100 mV; the load is comparator inputs, not
the 1k sense-divider legs. At the monitored 3 V boundary it has substantial
headroom, and the comparators remain in their specified supply range.
REF startup to 0.1% with 10 uF is 2.5 ms typical, NOT a maximum for this
fitted capacitor. Require reference/detector validity within 10 ms after
main first reaches 2.7 V. U2's minimum 12 ms delay supplies the cold-start
guard, including a slow/stalled ramp; radio release remains MCU/host gated.
[REF34 Rev G, sections 6.5 and 8](https://www.ti.com/lit/ds/symlink/ref34.pdf).

The new circuit does not spend the held-island reserve or modify U16/Q3.
No reference is powered from AON_HOLD, which would otherwise create a
back-power path into an unpowered comparator. The main-powered reference
capacitor still stores charge; its input limiters bound late input injection.
Include all added reference, timing and bypass capacitance in PWR-006's
final storage inventory and 1 mF whole-main-rail discharge ceiling. The
250 uF effective local minimum used below is this proposal's separate
fault-slew acceptance condition, not an inherited regulator qualification.
PWR-004's 250..850 uF model window is not a TPS63806 stability proof.
Ioff behavior of new LVC outputs does not independently qualify the
pre-existing U6 off-state paths.

Allocate 25 mA INSIDE the 1.8 A continuous main-load budget for this complete
addition, not on top of it. The bound is 84.825312 mW at maximum rail. Each
fast divider can draw 2.781433 mA; include the radio divider in the selected
switch's 20 mV complete-path loss calculation. The radio 0.505 A functional
screen includes approximately 0.5 A module load plus this small added load.

## Threshold and response allocations

| Result | Conditional calculated bound |
| --- | ---: |
| Fast nominal midpoint | 3.065 V |
| Minimum falling threshold | 3.030764284 V |
| Maximum clearing threshold | 3.099397864 V |
| Minimum rail at 5 mV input overdrive | 3.024622927 V |
| MCU release margin | 52.421816 mV |
| Radio release margin after 20 mV path loss | 32.421816 mV |
| MCU protected-pin supply after 50 ns response allocation | 3.024109983 V |
| Radio protected-pin supply after 50 ns response allocation | 3.018215135 V |

The reference error stacks its initial limit, box drift across the full
165 C datasheet interval, line regulation from the 2.55 V test supply to
maximum main voltage, and an allocated +/-0.05% combined assembly/aging
term. This is not an unlimited-lifetime promise.

Comparator error stacks 6.5 mV initial offset, an allocated 1 mV temperature
change, full 2.7..5.5 V PSRR excursion, 55 dB minimum CMRR at 2.5 V common
mode, 0.2 mV allocated reference/routing noise, and +/-50 nA at each input.
Allocate at most 10 mV total hysteresis, represented as +/-5 mV about the
offset midpoint. The 1 mV drift, 10 mV hysteresis, input-current, noise and
assembled-reference allowances require qualification; do not present them
as catalog maxima.

Allocate 50 ns from reaching the specified 5 mV overdrive to the protected
pin crossing its valid-low threshold. Include sense RC, comparator, logic,
routing and receiver capacitance. Sense-pin capacitance is limited to 10 pF,
comparator output load to 17 pF and protected reset-node load to 50 pF.
The sense RC time constant screen is 11.903 ns. Step-test propagation limits
are supporting evidence, not proof of response to every falling ramp.

Use main removal current <=2.25 A plus existing discharge, with >=250 uF
effective storage. Use radio total removal current <=0.60 A and effective
storage >=10.2uF*0.9*0.85*0.6 = 4.6818 uF. The latter is not 10.2 uF
effective. It includes QOD, divider and operating current. An external QOD
series resistance with minimum >=35.716 ohm would bound the extra current
above the 0.505 A functional screen, even assuming zero internal QOD
resistance. Final U4/QOD implementation must honor this current condition.
The radio time available from the overdrive point to 3 V is 192.133 ns.

## Complexity review: preserve as fallback, not preferred adoption

This implementation adds TEN ICs beyond the U2 replacement: two comparators,
one reference, two direct dual buffers, two monostables, two extension dual
inverters and one radio arbitration buffer. There are also numerous passives.
The 25 mA allocation would consume 600 mAh per day at the main rail if used
continuously. That is a budget ceiling, not predicted quiescent consumption;
even the two comparators' 6.4 mA combined typical current plus the low-value
dividers is undesirable during long main-powered reading/idle intervals.
Main-off sleep does not incur this added held-domain current.

The radio's tiny effective storage drives the nanosecond requirement. Under
this fallback's bounded fault model, the MCU channel has about 2.400 us from
its overdrive point to 3 V, while radio has only 0.192 us. Applying radio's
50 ns target universally to MCU is conservative, not a CPU requirement.
An integrated fast supervisor with a valid maximum delay at small overdrive
could replace the MCU comparator/reference/clamp chain. A part specified only
at 5% overdrive cannot establish a before-3-V guarantee from a 3.05 V trip:
3.05*0.95 = 2.8975 V even before propagation. A headline typical delay is
not enough. If the actual policy allows reset below 3 V while forbidding
SDRAM use/data retention there, the general MCU supply limit gives a much
larger window; that is an architecture decision, not an automatic relaxation.

A dual lower-power comparator is a credible second direction. TI TLV3202
offers two channels, a 6 mV full-temperature offset limit and tens-of-uA
quiescent-current figures rather than mA. Its published full-temperature
propagation rows reach 55 ns; overdrive/test-condition applicability across
the intended supply range, hysteresis (typical only), common-mode error,
and reset pulse capture still require a complete revised contract.
[TLV3202 Rev C, sections 6.5-6.8](https://www.ti.com/lit/ds/symlink/tlv3202.pdf).
No exact TLV3202 ordering code, divider or replacement is selected here.

A dual comparator plus independently released reset latches may remove the
two one-shots and some output buffers. It must have deterministic startup,
asynchronous assertion, bounded pulse capture, delayed release independent
of its own asserted output, and no MR/RESET feedback latch. A shared fault
latch must not make intentionally switching off radio reset the MCU. Such
a design has not yet been proven; preserving the explicit fallback is not
evidence that the simpler alternative fails.

Before adopting this fallback, prefer a bounded comparison using actual
main-rail discharge and permitted SDRAM fault behavior, plus radio QOD and
local-capacitance choices. Do not demand survival of universal instantaneous
shorts, nor remove the genuine normal-operation 3 V memory/radio constraints.
No replacement is adopted by this complexity assessment.

## Procurement snapshot

Checked 2026-09-08; indexed, unreserved distributor quantities, not order
quotes. Existing retained parts and reused capacitors retain their earlier
documented sourcing dates.

| Exact part | Indexed quantity | Source |
| --- | ---: | --- |
| TPS3808G01DBVR | 101194 | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TPS3808G01DBVR/666712) |
| RT0603BRD0761K9L | 3617 | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RT0603BRD0761K9L/5139152) |
| RT0603BRD0710KL | 145117 | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RT0603BRD0710KL/1072198) |
| RT0603BRD0732K4L | 4947 | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RT0603BRD0732K4L/5139084) |
| RT0603BRD07226RL | 6424 | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RT0603BRD07226RL/1072366) |
| RG1608P-102-B-T5 | 489725 | [DigiKey](https://www.digikey.com/es/products/detail/susumu/RG1608P-102-B-T5/1240449) |
| TLV3501AIDBVR | 24868 | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TLV3501AIDBVR/1669420) |
| REF3425IDBVR | 5826 | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/REF3425IDBVR/7899492) |
| SN74LVC2G07DCKR | 155249 | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC2G07DCKR/486430) |
| SN74LVC2G06DCKR | 83815 | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC2G06DCKR/486426) |
| SN74LVC1G123DCTR | 83061 | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC1G123DCTR/863597) |
| SN74LVC1G07DBVR | 148017 | [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/SN74LVC1G07DBVR/377455) |
| RC0603FR-074K7L | 1474011 | [DigiKey](https://www.digikey.com/en/products/detail/yageo/RC0603FR-074K7L/727212) |

## Reproducible calculation

This executes arithmetic only. It does not generate circuit files, simulate
the switching regulator, or establish hardware acceptance.

```sh
python3 - <<'PY'
from math import isclose

fl = .999*.9975*.9985
fh = 1.001*1.0025*1.0015
vmin, vmax = 3.151819680, 3.393012496

def supervisor(v, accuracy, hysteresis, rt, rb, leakage):
    assert v > 0 and rt > 0 and rb > 0
    low = v*(1-accuracy)*(1+rt*fl/(rb*fh))-leakage*rt*fh
    high = v*(1+accuracy)*(1+hysteresis)*(1+rt*fh/(rb*fl))
    high += leakage*rt*fh
    assert 0 < low < high
    return low, high

mcu = supervisor(.405, .02, .03, 61900, 10000, 75e-9)
radio = supervisor(1.15, .01, .00825, 32400, 20000, 150e-9)
ratio_low = 1+226*fl/(1000*fh)
ratio_high = 1+226*fh/(1000*fl)
ref_error = .0005+6e-6*165+15e-6*(vmax-2.55)+.0005
input_error = (
    .0065+.001+2.5/10**(55/20)+400e-6*2.8
    +.0002+2*50e-9*1000*fh
)
fall = (2.5*(1-ref_error)-input_error-.005)*ratio_low
fall -= 50e-9*226*fh
clear = (2.5*(1+ref_error)+input_error+.005)*ratio_high
clear += 50e-9*226*fh
detect = fall-.005*ratio_high

radio_cap = 10.2e-6*.9*.85*.6
radio_slew = .6/radio_cap
main_slew = (2.25+vmax/10.7811)/250e-6
main_pin = detect-main_slew*50e-9
radio_pin = detect-radio_slew*50e-9
cmin = .1e-6*.95*.997*.999
cmax = .1e-6*1.05*1.003*1.001
pulse_min = 10000*fl*cmin
pulse_max = 1.1*10000*fh*cmax

checks = {
    'MCU supervisor fall V': (mcu[0], 2.8245995411747034),
    'MCU supervisor release V': (mcu[1], 3.090430600817248),
    'radio supervisor fall V': (radio[0], 2.959633849841741),
    'radio supervisor release V': (radio[1], 3.0921868948645885),
    'fast fall V': (fall, 3.0307642835712394),
    'fast clear V': (clear, 3.09939786406335),
    '5mV overdrive rail V': (detect, 3.0246229268672162),
    'MCU release margin V': (vmin-clear, .052421815936650074),
    'radio release margin V': (vmin-.020-clear, .032421815936650056),
    'MCU pin supply after 50ns V': (main_pin, 3.024109983150972),
    'radio pin supply after 50ns V': (radio_pin, 3.0182151349922965),
    'radio time available ns': ((detect-3)/radio_slew*1e9, 192.13269834488827),
    'pulse minimum s': (pulse_min, .0009414791652738268),
    'pulse maximum s': (pulse_max, .0011654305737554412),
    'sense RC ns': ((1000+226*1000/1226)*fh*10e-12*1e9,
                    11.902701944902118),
    'off-state input current mA': ((vmax/ratio_low-.3)/(1000*fl)*1000,
                                   2.485038427823708),
    'fast divider current A': (vmax/(1226*fl), .0027814325141257696),
    'new circuit power allocation W': (vmax*.025, .0848253124),
    'MR high V': (3-100e-6*4700*1.01*1.01*1.0015, 2.5198338295),
    'MR sink A': (vmax/(4700*.99*.99*.9985)+100e-6,
                  .0008376819257459044),
    'QOD series minimum ohm': (vmax/(.6-.505), 35.71592101052632),
}
for name, (actual, expected) in checks.items():
    assert isclose(actual, expected, rel_tol=1e-11, abs_tol=1e-12), name
    print(name, actual)
assert max(mcu[1], radio[1]) < clear
assert vmin-.020-clear > .032 and radio_pin > 3.018 and main_pin > 3.024
assert pulse_min > .0009 and pulse_max < .0012
assert checks['off-state input current mA'][0] < 10
assert checks['MR high V'][0] > .7*3
assert checks['MR sink A'][0] < .001
print('MCU time available us', (detect-3)/main_slew*1e6)
print('continuous 25mA allocation mAh/day', .025*1000*24)
print('Arithmetic PASS; proposal remains unadopted and unqualified.')
PY
```
