# ADR-0007: Bound the warm/cool front-light channels to a two-sink boost driver fed from the battery

## Status

Proposed. The electrical decision below is analysis only; every photometric
and thermal number it depends on needs bench confirmation on Rev 1 hardware
(see Open questions). Tracks #831 (parent #821).

Numbering note: `0005` is claimed by two open pull requests at the time of
writing (#1225 SDRAM interface supply domain, #1227 usb_cdc example
duplication). This ADR takes `0007` to leave room for both; renumber on the
merge train if either lands differently.

## Context

#831 asks for "smooth, efficient, independently adjustable warm and cool
illumination" for the final light guide, and its acceptance criteria demand
quantitative brightness and mixing ranges, a stated control resolution, and
checked sleep leakage. None of that can be settled by picking a part number
late in layout: the driver's per-sink current ceiling and its series-LED
ceiling are *constraints on the light guide*, so they have to be written
down before #822 commits to a light guide and before #836 routes anything.

Three facts about the current tree shape this decision.

1. **Nothing in firmware drives a front light today.** A repository-wide
   search for `frontlight` / `front_light` returns zero hits, and
   `libs/ra8_board_ra8p1` knows only three user LEDs
   (`k_ra8_board_led1..3`, provisional pins P600 / P303 / PA07, themselves
   `TODO(EK-RA8P1 UM / ra8p1_kicad)`-marked as mirrored from the EK-RA8D2).
   Those are indicator LEDs on GPIO, not an illumination channel. So no
   firmware assumption constrains the choice yet, and whatever is chosen
   has to arrive with a board-layer seam rather than slot into one.
2. **The main rail is already spoken for.** ADR-0005 (#846, PR #1225)
   bounds any MCU domain carrying SDRAM DQ bits to a complete envelope
   inside 3.00 V..3.35 V, leaving >= 55 mV of guaranteed read-high margin
   at the ceiling. A front light is the largest dynamic load on a reader,
   so where it takes its input from is an SDRAM-margin question, not just a
   power-tree preference.
3. **Two channels means two independently regulated sinks**, not one sink
   and a switch. Warm/cool mixing is a current ratio held across the whole
   brightness range, so the channels must be separately programmable and
   their matching must be specified, otherwise a nominal 50/50 mix drifts
   in colour as brightness changes.

## Decision

### 1. Topology: one boost, two independently programmed high-voltage sinks

Baseline part: **TI LM3630A** (SNVS974B, April 2013, revised October 2015),
a current-mode boost driving up to two strings of 10 series LEDs with
independent current control per string and per-bank programming over I2C.
Warm goes on one sink/bank, cool on the other.

What that locks in, from the LM3630A Electrical Characteristics table
(TA = 25 degC typicals, limits over -40..+85 degC, VIN = 3.6 V unless
noted). Each line is the datasheet figure, then the design consequence.

* Full-scale current per sink, 5 mA .. 28.5 mA (Description, p.1): a hard
  ceiling of 28.5 mA per colour on the light guide.
* Series LEDs per string, up to 10 (Features, p.1): a hard ceiling of 10
  series LEDs per colour.
* Output current regulation, 19 / 20 / 21 mA at 20 mA full scale: +/- 5%
  absolute set-point accuracy.
* Sink-to-sink matching at ILED = 10 mA, -1% / 0.5% / +1% at 25 degC and
  -2.5% / +2.5% over 0..70 degC with ILED1 on bank A and ILED2 on bank B:
  the mixing ratio holds to about 2.5% worst case at 10 mA, and is
  unspecified at the bottom of the range.
* Brightness control, 8-bit, 256 exponential or linear steps per bank:
  the control resolution behind the mixing targets below.
* Minimum LED current, 13 uA (full scale 20 mA, BRT = 0x01, exponential):
  a night-reading floor at about 0.065% of full scale, set in software.
* Shutdown current, 1 uA typ and 4 uA max (HWEN = GND, and separately with
  HWEN = VIN in I2C shutdown): the sleep-leakage budget below.
* Quiescent current not switching, 350 uA typ (ILED1 = ILED2 = 20 mA,
  feedback disabled): do not leave the part enabled-but-idle.
* Current-sink headroom, VHR 160 mV typ / 240 mV max at ILED = 20 mA, and
  VREG_CS 250 mV at 5 mA: boost output target is Vf_string + 240 mV worst
  case.
* OVP options, 16 / 24 / 32 / 40 V, with the 24 V option at 23/24/25 V and
  the 40 V option at 39/41/44 V: choose per final string length, against a
  45 V absolute maximum on SW / OVP / ILEDn.
* Switching frequency, 500 kHz (481/500/518) or 1 MHz (962/1000/1038),
  each with a +10% shift option: both well above the audible band.
* Maximum duty cycle, 94%: the boost compliance limit at low battery.
* PWM input frequency, 10 kHz .. 80 kHz: if PWM is used at all it has to
  stay inside that window.
* Logic thresholds, VIL <= 0.4 V and VIH >= 1.2 V: works from a 1.8 V or a
  3.3 V MCU domain.
* I2C address, 0x36 (SEL = GND) or 0x38 (SEL = VIN): a strapping
  constraint, see section 4.
* Initialisation, tWAIT 1 ms minimum from HWEN assert (or software reset)
  before an I2C transaction is ACKed, earlier ones NAKed: firmware-visible
  timing.
* Thermal shutdown, 140 degC typ with 15 degC hysteresis: feeds the
  thermal review under #835 and #836.

**Alternative considered: TI LM3697** (SNOSCS2D, November 2013, revised
March 2019). Three sinks at up to 30 mA, 11-bit (2048-step) dimming, PWM
usable over 2 kHz..100 kHz, shutdown 1 uA typ / 3 uA max, minimum LED
current 6 uA typ at 20.2 mA full scale exponential, OVP up to 40 V with an
integrated 1 A / 40 V FET. It is the better part if the light guide turns
out to need more than 28.5 mA per colour or a finer bottom end. Two reasons
it is not the baseline: its recommended input range starts at 2.7 V against
the LM3630A's 2.3 V, which costs usable margin at end-of-discharge, and its
third sink is dead weight for a two-colour light guide. Its low-current
matching spec is the cautionary data point for either part: +/- 8.5% at
ILED = 500 uA against +/- 1.7..2.5% at 20.2 mA. Sink matching degrades by
an order of magnitude at the bottom of the range, which is exactly where a
reader spends its night hours.

### 2. Input: from the battery, never from the SDRAM-domain rail

The LM3630A's recommended input range is 2.3 V..5.5 V, which covers a
1-cell Li-ion across its whole discharge curve. It therefore takes VBAT,
not the regulated main rail.

The reason is margin arithmetic, not tidiness. Worst case at the driver's
own ceiling, with a 10-series string of nominally 3.0 V white LEDs on both
channels:

```
Vout(worst) = 10 x 3.0 V + 0.240 V headroom          = 30.24 V
Pout(worst) = 2 channels x 28.5 mA x 30.24 V         =  1.724 W
Pin at 87%  (datasheet headline "Up to 87% Efficient") =  1.982 W
Iin at 3.40 V                                        =    583 mA
Iin at 3.00 V                                        =    661 mA
```

A realistic reading point, two channels at 10 mA into a 6-series string:

```
Vout        = 6 x 3.0 V + 0.240 V                    = 18.24 V
Pout        = 2 x 10 mA x 18.24 V                    =  0.365 W
Pin at 85%                                           =  0.429 W
Iin at 3.70 V                                        =    116 mA
```

Every figure above is arithmetic from datasheet limits plus an assumed
3.0 V per-LED Vf. None of it is a measurement; no board exists. The 87%
and 85% efficiency figures are the datasheet's headline and a conservative
shading of it, not efficiency at our operating point.

That arithmetic is the decision: a load that steps to a few hundred
milliamps, switching at 500 kHz or 1 MHz, must not share the regulated
domain that ADR-0005 holds inside 3.00..3.35 V with >= 55 mV of guaranteed
read-high margin. The input-current ripple and the step load would be spent
straight out of that 55 mV. Feeding the driver from VBAT keeps the
disturbance on the cell and out of VCC/VCC2. If #825 later insists the
front light hang off a regulated rail, that rail is a separate regulator
from VCC/VCC2, and ADR-0005's envelope stands unchanged.

Sleep leakage, for the #831 acceptance criterion: 4 uA maximum with HWEN
low, plus the part's true shutdown isolation on the LED strings, so the
light guide contributes no separate leakage path. Against a 2000 mAh cell
that is under 0.2% of capacity a year. That is a datasheet limit, not a
measured standby draw.

### 3. Control ownership: I2C DC dimming; the PWM pin is not used

Brightness and mixing are set by writing the per-bank brightness registers
over I2C. The PWM input is strapped off in Rev 1.

Three reasons. The 8-bit exponential map already reaches a 13 uA floor, so
PWM buys no range. Any duty-cycle dimming re-introduces a flicker and
camera-banding question that DC dimming does not have. And the PWM window
is 10 kHz..80 kHz, so a naive few-hundred-hertz dimming signal is out of
spec and risks audible response from the output ceramics; not offering the
pin removes the chance of that mistake. The trade is that smoothing (ramps,
soft transitions) becomes the driver's internal soft-start plus firmware
stepping rather than a hardware duty cycle.

Quantitative control targets for the acceptance criteria, stated against
the part rather than against a panel that does not exist yet:

* Per-channel range 13 uA .. 28.5 mA, about 67 dB of electrical range, in
  256 exponential steps per channel.
* Mixing resolution 256 x 256 warm/cool pairs, with ratio accuracy bounded
  by sink matching: about 2.5% worst case at 10 mA per channel, and
  unbounded by datasheet below roughly 1 mA (the LM3697's +/- 8.5% at
  500 uA is the comparison).
* Minimum stable brightness set by whichever comes first, the 13 uA
  current floor or the LED's own useful emission floor. The electrical
  floor is known; the photometric floor is a bench measurement.

### 4. Pin and rail inventory

Board side, as schematic deliverables for #834 and layout constraints for
#836:

* `IN` from VBAT, 2.2 uF minimum ceramic to GND at the pin (pin table).
* `SW` to the boost inductor. The datasheet's typical characteristics are
  taken at 10 uH and 22 uH; the final value waits on the real string and
  #836's PI review.
* Schottky to the output node, output capacitor, and `OVP` sensed at the
  output node's positive terminal.
* `ILED1` as the warm sink and `ILED2` as the cool sink, one string each,
  to the light connector defined by #830.
* `HWEN` from an MCU GPIO, held low for shipping and deep sleep.
* `SCL` / `SDA` on the shared house I2C bus.
* `SEL` strapped to GND for 7-bit address 0x36.
* `PWM` strapped off, per section 3.
* SW / OVP / ILED1 / ILED2 all carry up to 45 V absolute maximum: a
  high-voltage island in a hand-held enclosure, so keep it away from the
  capacitive-touch sense lines of #830.

The `SEL` strap is not a preference. 0x38 is the stock 7-bit address of the
FT5x06-class capacitive touch controllers that #830 is likely to reach for,
so leaving `SEL` at VIN risks an address collision on a shared bus. If the
final touch part sits elsewhere, revisit; the safe default is 0x36.

Firmware side, not implemented here, recorded so it is not discovered late:

* A front-light seam is needed in the board layer; nothing exists to
  extend. `ra8_board_led_*` is three GPIO indicators and is not it.
* The board layer has to publish an I2C bus handle for the front light and
  own the 0x36 address constant.
* tWAIT of 1 ms minimum from HWEN assert to the first ACKed I2C
  transaction is a real sequencing requirement on whatever brings the
  light up.
* Warm/cool ratio and brightness belong in one call, not two: a
  per-channel API invites transient colour shifts mid-adjustment.

## Consequences

* #822 inherits two hard numbers when it selects the light guide: at most
  10 series LEDs and at most 28.5 mA per colour. A guide that wants
  parallel strings per colour, or more than 28.5 mA, invalidates the
  baseline part, and the LM3697 at 30 mA only marginally relieves it. That
  is a vendor conversation, not something to discover at ERC.
* #825's power tree gains a VBAT-fed branch that can pull several hundred
  milliamps at the ceiling, and does not gain a front-light load on
  VCC/VCC2. ADR-0005's 3.00..3.35 V envelope is unaffected by this
  decision, which is the point of it.
* #830's touch design inherits the 0x36 / 0x38 address constraint and a
  45 V-capable island near the FPC and sense lines.
* Rev 1 carries no hardware dimming path. If bench work finds DC dimming
  insufficient (an LED whose spectrum shifts with current, say), the PWM
  pin is a stuff option, so leave the net and a 0R to ground rather than
  tying it hard.
* Nothing here selects an inductor, a Schottky, an output capacitor, or an
  LED. Those stay open until #822 delivers the string, and they belong to
  #834 and #836.

## Open questions (needs bench, or needs #822)

1. String topology, from #822: LED count per colour, series/parallel
   arrangement, Vf range and binning, and whether warm and cool differ in
   Vf enough to matter (they usually do, the phosphor and drive differ).
   Every voltage number above assumes 3.0 V per LED, which is an
   assumption, not a datasheet value for a part anyone has chosen.
2. Efficiency at our operating point. 87% is the LM3630A's headline figure
   at a particular VIN and string. Ours is unmeasured.
3. Thermal rise. A 12-bump DSBGA moving up to about 1.7 W of output power
   sits behind a panel in a sealed hand-held, against a 140 degC typical
   thermal shutdown. Needs bench, and a thermal opinion from #835 before
   the stackup closes.
4. Perceived flicker and camera banding under DC dimming. Expected to be a
   non-issue by construction, but it is an acceptance criterion in #831 and
   only a bench check closes it.
5. Audible noise from the output ceramics under fast firmware brightness
   ramps. The switcher itself is at 500 kHz or 1 MHz, well clear of the
   audible band; the risk is the envelope, not the carrier.
6. Colour temperature versus channel ratio. Photometric, not electrical.
   The warm/cool mixing target in #831's acceptance criteria cannot be
   stated in Kelvin until the guide and LEDs are measured.
7. Channel matching at the bottom of the range. The LM3630A specifies
   matching only at ILED = 10 mA. Below roughly 1 mA, expect visible colour
   drift between channels, and decide whether the firmware floor should sit
   above the electrical floor.
8. Inductor and capacitor selection across the battery range and LED
   tolerances, including switching-loop layout constraints and test points.
   #831's scope asks for these; they are blocked on item 1.

## References

* TI LM3630A, *High-Efficiency Dual-String White LED Driver*, SNVS974B,
  April 2013, revised October 2015. Features and Description p.1; Pin
  Functions; Absolute Maximum Ratings and Recommended Operating
  Conditions; Electrical Characteristics (output current regulation,
  IMATCH, VREG_CS, VHR, ICL, VOVP, fSW, DMAX, IQ, ISHDN, ILED_MIN, TSD,
  tWAIT); logic and PWM characteristics (VIL, VIH, fPWM).
* TI LM3697, *High-efficiency three-string white LED driver*, SNOSCS2D,
  November 2013, revised March 2019. Features p.1; Recommended Operating
  Conditions; Electrical Characteristics (ISHDN, ILED_MIN, IHVLED,
  IMATCH_HV, VREG_CS, VOVP); PWM Input Frequency Range; 11-bit code
  calculation.
* ADR-0005 (PR #1225, issue #846): SDRAM interface supply domain, the
  3.00..3.35 V envelope with >= 55 mV guaranteed read-high margin.
* Tree, at dev 013631d: `libs/ra8_board_ra8p1/inc/ra8_board_ra8p1.h` and
  `src/ra8_board_ra8p1.c` (three provisional GPIO user LEDs, no
  illumination channel); no `frontlight` / `front_light` symbol anywhere in
  the repository.
* Issues: #831 (this decision), #821 (parent epic), #822, #823, #825,
  #830, #834, #835, #836, #846.
