# E-reader audio subsystem: engineering basis

Revision 1, 2026-09-07. Tracking: [audio #840](https://github.com/bsikar/ra8-firmware/issues/840)
under [e-reader #821](https://github.com/bsikar/ra8-firmware/issues/821).
Cross-reference: [system power](system_power_design.md),
[single-button power](single_button_power.md), and
[native root schematic](../ereader/ereader_rev1.kicad_sch).

This record defines the circuit direction, procurement candidates and
reproducible calculations. It is not an implemented schematic or a claim
of measured performance. Figures marked target, allocation or example are
engineering choices, not IC guarantees. Circuit values and native-sheet
references must be added as each stage is implemented and reviewed.

## AUD-001: Required interfaces and proposed performance envelope

The product requires both a 3.5 mm single-ended headphone output and a
4.4 mm balanced headphone output, local-file playback from microSD,
USB-C DAC operation, and built-in speakers. Use a separate low-noise
headphone mode and high-power mode. No single power or noise specification
establishes compatibility with every headphone: impedance, sensitivity,
frequency-dependent load, cable capacitance and required listening level
all matter.

Proposed qualification targets, not yet proven or approved output ratings:

| Mode or property | Initial target and measurement conditions |
| --- | --- |
| PCM interface | Stereo 44.1/48/88.2/96/176.4/192 kHz; 24-bit samples in 32-bit I2S slots |
| Low-gain 3.5 mm | Up to 1 Vrms where load permits; noise <=1 uVrms A-weighted; also report unweighted 20 Hz..20 kHz noise |
| Higher-gain 3.5 mm | Voltage/current envelope to be qualified separately; do not copy the balanced rating |
| Balanced, 16 Ohm | 0.25 W per channel, both channels driven |
| Balanced, 32 Ohm | 0.5 W per channel, both channels driven; 1 W per channel is a stretch, not the baseline |
| Balanced, high impedance | 8 Vrms differential into >=300 Ohm; check real output swing and supply sag |
| THD+N | <=-110 dB at 1 kHz, 100 mW into 32 Ohm; report 1 mW noise-limited performance separately; <=-100 dB over 20 Hz..20 kHz at declared rated output; report bandwidth, gain and load |
| Frequency response | 20 Hz..20 kHz within +/-0.1 dB relative to 1 kHz, with digital-filter mode and complex test load stated |
| Output impedance | <=0.5 Ohm single-ended; <=1 Ohm balanced for the reference parallel stage; <=0.5 Ohm balanced is a separate stability/sharing improvement |
| Channel isolation | >=90 dB at 1 kHz loaded; also characterize at 20 kHz |
| Speakers | Stereo, initial electrical ceiling 2 x 1 W into nominal 8 Ohm, subject to actual transducer and enclosure qualification |

THD+N must not be silently substituted with A-weighted dynamic range.
Measure clipping versus frequency and load, not only a 1 kHz headline.
Do not advertise 32-bit analog resolution because the transport uses
32-bit slots. DSD/DoP is optional and not included in this baseline.
The 44.1 kHz digital filter must be checked at 20 kHz before promising
the response target for every filter setting.

## AUD-002: Architecture and part-selection basis

```text
microSD -> RA8P1 PCM buffer <- USB high-speed audio OUT + feedback IN
                   |
                 SSI1 TX <--- BCLK/LRCLK from DAC clock-output mode
                   |
          +--------+--------------------+
          |                             |
     ES9039Q2M                     2 x TAS2563
     DAC + I/V/filter              internal DSP/boost/Class-D
          |                             |
     +----+---------------+          L/R speakers
     |                    |
 low-gain SE          differential high-current
 OPA1622             parallel INA1620 stages
     |                    |
 protected 3.5 mm     protected 4.4 mm
```

Use one active listening output at a time. Two jacks inserted defaults to
mute or an explicit user-selected output, never uncontrolled simultaneous
drive. Speaker output is disabled when either headphone output is selected.
This is an output policy, not an assumption that analog outputs can be
connected together or that disabled IC outputs are high impedance.

The preferred DAC candidate is ESS ES9039Q2M. ESS lists it active for new
two-channel designs. Use the manufacturer-authored
[ES9039Q2M v0.2.2 datasheet](https://www.mouser.com/datasheet/3/3763/1/ES9039Q2M_Datasheet_v0.2.2.pdf),
especially the output-stage reference on p. 98. Its OPA1612 compensation
components belong to that circuit; they must not be copied to a different
DAC or arbitrary op amp. The evaluation-board dynamic-range headline uses
a 10 Vrms full-scale measurement, not a guaranteed 2 Vrms headphone output.
Consequently it cannot establish this product's low-gain noise floor.

Use a dedicated DAC rather than paying for an unused ADC in a codec.
[CS43131](https://statics.cirrus.com/pubs/proDatasheet/CS43131_DS1155F2.pdf) remains a strong
low-power integrated alternative, but does not establish the requested
high-power balanced envelope. [TAC5212](https://www.ti.com/product/TAC5212)
offers an integrated signal chain, but its line-output specifications also
do not replace a load-qualified headphone stage. Keep these as architectural
alternatives, not simultaneously populated DACs.

The candidate balanced stage uses four INA1620 dual ICs: two amplifiers
in parallel for each of four driven legs. TI's
[SBOA352 parallel headphone-amplifier reference](https://www.ti.com/lit/pdf/sboa352)
uses a gain amplifier followed by a unity-gain current-sharing amplifier,
with an individual 1 Ohm output resistor for each amplifier. Its feedback
is taken before the ballast resistor. Two resistors in parallel produce
0.5 Ohm per leg, or 1 Ohm differential. The documented reference result
does not qualify a different rail voltage, ballast value or BTL arrangement.
Validate offset mismatch, current sharing, clipping recovery and capacitive
loads before modifying the ballast or paralleling additional stages.

The [INA1620 pin table](https://www.ti.com/lit/gpn/INA1620) distinguishes
GND pin 3 from the exposed thermal pad: the pad connects to the most
negative supply, not ground. OPA1622 also requires its thermal pad at V-.
These are electrical net assignments that must be represented correctly
even while PCB layout and final footprint review are deferred.

OPA1622 is a candidate for the independent low-gain single-ended path.
The [OPA1622 datasheet](https://www.ti.com/lit/ds/symlink/opa1622.pdf)
lists +145/-130 mA as typical short-circuit currents, not guaranteed
low-distortion drive limits. One amplifier per balanced leg must not be
claimed to deliver 1 W into 32 Ohm. Disabled output impedance is not a
specified isolation switch; add a separately qualified output-disconnect
mechanism if required by jack selection and fault behavior.

[BUF634A](https://www.ti.com/lit/ds/symlink/buf634a.pdf) is a possible
composite-buffer alternative, but its typical current and headroom figures
still need a full feedback/stability/thermal design.
[TPA6120A2](https://www.ti.com/lit/ds/symlink/tpa6120a2.pdf) has substantial
drive capability, but its recommended series output resistance conflicts
with the low-source-impedance objective unless separately compensated.
Neither is an approved drop-in solution.

## AUD-003: Load, gain, current and noise calculations

For a sine wave into a resistive load R:

```text
V_rms = sqrt(P_load * R)
I_rms = V_rms / R
I_peak = sqrt(2) * I_rms
V_leg,peak = V_rms / sqrt(2)       [symmetric balanced output]
I_each,peak = I_peak / 2          [ideal two-amplifier sharing per leg]
```

| Differential load case | V_rms | I_peak through headphone | Peak per amplifier, ideal sharing | Peak voltage per leg |
| --- | ---: | ---: | ---: | ---: |
| 16 Ohm, 0.25 W | 2.000 V | 176.777 mA | 88.388 mA | 1.414 V |
| 32 Ohm, 0.5 W | 4.000 V | 176.777 mA | 88.388 mA | 2.828 V |
| 32 Ohm, 1 W stretch | 5.657 V | 250.000 mA | 125.000 mA | 4.000 V |
| 300 Ohm, 8 Vrms | 8.000 V | 37.712 mA | 18.856 mA | 5.657 V |
| 600 Ohm, 8 Vrms | 8.000 V | 18.856 mA | 9.428 mA | 5.657 V |

For 300/600 Ohm, the last two rows dissipate 213.333/106.667 mW per
headphone channel. Include ballast drop, output headroom and worst rail
droop above the leg voltage; +/-5 V cannot produce 8 Vrms differential.
For the 1 W stretch case, even ideal sharing demands 125 mA peak per
amplifier. Hot-device linear-current margin is therefore a release gate,
not satisfied by a typical short-circuit number.

Output-resistance error is load dependent:

```text
attenuation_dB = 20*log10(R_load / (R_load + R_source))
```

0.5 Ohm into 16 Ohm gives -0.2673 dB; into 32 Ohm, -0.1347 dB.
1 Ohm balanced into 16 Ohm gives -0.5266 dB. Pure resistive attenuation
can be calibrated; a headphone's changing impedance can turn that divider
into frequency-response error. This is why source impedance is specified
separately from unloaded frequency response.

For sensitivity S expressed in dB SPL per mW and an illustrative acoustic
level L, P_mW = 10^((L-S)/10). Then V_rms = sqrt(P_mW*0.001*R).
If sensitivity is instead dB SPL per volt, use
V_rms = 10^((L-S_V)/20). Do not mix the two sensitivity units. These are
electrical sizing examples, not recommended listening levels. No headphone
sensitivity has been selected for this product.

For example, 1 uVrms electrical noise into 16 Ohm with hypothetical
115 dB SPL/mW sensitivity corresponds to 12.96 dB SPL using this simplified
power conversion. Noise spectrum, weighting and acoustic response matter;
even the 1 uV target does not establish silence for every sensitive IEM.

An independent-noise allocation uses:

```text
e_out = sqrt((G_signal*e_DAC)^2 + e_amp,out^2 + e_resistor,out^2 + ...)
DNR_dB = 20*log10(V_fullscale,rms / e_out)
```

Amplifier noise gain is not necessarily its signal gain. Resistor Johnson
noise, current-noise times source impedance, low-frequency noise and
filter bandwidth must be included in the actual circuit integral. As an
allocation example only, e_DAC = 0.7 uV, G = 0.25 and e_amp,out = 0.4 uV
give 0.4366 uV before additional resistor/supply/switch contributions.
These are not measured ES9039Q2M or OPA1622 values. Digital attenuation
does not remove the DAC's analog noise floor; use a real low analog gain
for IEMs and sequence gain changes under mute.

At 1 mW into 32 Ohm, the signal is only 0.1789 Vrms. Even a 1 uVrms
noise floor alone gives -105.05 dB noise-to-signal, before distortion.
Therefore the -110 dB target at 100 mW must not be advertised as applying
down to 1 mW in every gain mode.

## AUD-004: Clock and USB-audio contract

RA8P1's [Hardware User's Manual](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
R01UH1064EJ0130, Ch. 47 and Table 70.75, specifies an SSIE bit-clock
cycle of at least 80 ns in both clock directions. Thus BCLK <=12.5 MHz.
Stereo with two 32-bit slots requires BCLK = 64*Fs:
192 kHz gives 12.288 MHz and passes this limit; 384 kHz gives 24.576 MHz
and fails. DAC sample-rate capability does not override the MCU limit.
SSIE1 is half duplex, which is sufficient for this playback-only stream.
Use DMA and bounded buffers rather than expecting a foreground loop to
service each sample. Match the specified pin group for AC characterization.

Prefer dedicated 22.5792 MHz and 24.576 MHz low-phase-noise oscillators
with a qualified, muted clock-selection sequence. The DAC generates BCLK
and LRCLK while the RA8P1 accepts them as inputs and transmits sample data.
ESS supports the corresponding 128/256/512 clock ratios. The exact compact
oscillator and switch MPNs are not yet selected; no stock claim is made.
Do not short oscillator outputs together. I2C access, clock transitions,
DAC power sequencing and reset must follow the selected DAC mode.

This clock direction avoids relying on RA8P1's permitted 35% bit-clock
high/low duty interval to satisfy an ESS input-clock 45..55% requirement.
It does not close the entire timing budget. At 192 kHz, a nominal half
cycle is 40.690 ns; RA transmit delay up to 20 ns plus ESS data setup
4.1 ns leaves 16.590 ns before clock duty/skew/jitter allowances. Verify
DAC clock-output timing, the correct I2S edges, RA hold constraints,
trace skew and oscillator limits at voltage/temperature before approval.

One SSI1 transmit bus can feed the DAC and two speaker ICs. Use 48 kHz
in speaker mode and shut down the speaker outputs in headphone mode.
Keep speaker SDOUT disabled; host I/V telemetry is optional diagnostic
functionality, not a required fourth wire for playback. The internal
speaker-protection DSP still needs proper characterization and coefficients.
If the DAC supplies clocks in speaker mode, it remains powered and muted;
include that power until an independent clock solution is implemented.
Check digital input tolerance when any consumer is powered down, adding
qualified isolation if necessary. Do not assume shutdown equals powered-off
I/O tolerance. Proposed SSI1_A pins must be confirmed in the common pin map:
P907 BCLK, P906 LRCLK and P206 data; optional 8-bit eMMC conflicts with P206.

USB uses the MCU high-speed interface and a USB Audio Class 2 asynchronous
playback endpoint with explicit feedback. The
[USB-IF UAC2 specification with 2025 errata](https://www.usb.org/sites/default/files/Audio2_with_Errata_and_ECN_through_Apr_2_2025.pdf)
defines the class contract; the
[Microsoft USB audio driver requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)
require explicit feedback for asynchronous playback rather than implicit
feedback. Expose only implemented sample rates and one coherent logical
clock source across oscillator families. Measure sample consumption against
USB time; a DAC ASRC does not implement the USB feedback endpoint.

At 192 kHz, two channels and four bytes per slot require 1,536,000 bytes/s,
or nominally 192 bytes per 125 us high-speed interval. Full-speed USB's
1023-byte-per-ms isochronous ceiling cannot carry this format. Buffer and
descriptor sizing must include actual asynchronous rate variation, not
just the nominal packet. USB connect/suspend/resume and stream altsetting
zero must mute cleanly. The existence of a USB peripheral is not evidence
that class firmware or host interoperability is already implemented.

## AUD-005: Audio rails, thermal load and power-path impact

Derive headphone rails directly from the qualified system power path,
not by attaching the high-power stage to +3V3_MCU. Use low rail magnitude
for low-impedance loads and a higher magnitude for high-impedance voltage
drive; proposed screens are +/-5 V and +/-8 V. These are design screens,
not final regulator settings. Analog rail changes occur only with outputs
disconnected/muted and settled; cover asymmetric rail collapse.

For an ideal class-B BTL sine output, each leg draws one half-wave from
each supply. For two headphone channels and eight active amplifiers:

```text
P_rails = 2_channels * 4 * V_rail * I_peak / pi
          + 8_amplifiers * Iq_per_amp * (2 * V_rail)
P_battery = P_rails / eta
P_heat = P_battery - 2_channels * P_load
```

Using 2.6 mA typical amplifier quiescent current and an assumed 85%
converter efficiency gives the following screen. It excludes DAC, I/V,
clock, switches, ballast losses, post-regulators and current-sharing error;
it is neither worst case nor the complete product budget.

| Both channels driven | Rail magnitude | Rail input | Battery input | Heat excluding headphone load |
| --- | ---: | ---: | ---: | ---: |
| 0.5 W each into 32 Ohm | 5 V | 2.459 W | 2.893 W | 1.893 W |
| 1 W each into 32 Ohm, stretch | 6 V | 4.069 W | 4.787 W | 2.787 W |
| 1 W each into 32 Ohm, stretch | 8 V | 5.426 W | 6.383 W | 4.383 W |

At 3.0 V, the baseline audio screen alone draws 0.964 A from the source.
Adding the previous MCU/radio-only 3.286 W input screen gives 6.179 W,
or 2.060 A at 3.0 V, before other loads. That earlier source budget cannot
approve the new product. Reopen battery continuous/pulse rating, charger
power-path capability, input current negotiation, connectors, heating and
charging derating. Never promise full audio output while charging from an
unconfigured or low-current USB source. Pack supplementation requires a
present, sufficiently charged and qualified pack.

TPS65131WTRGERQ1 is a sourceable bipolar-converter candidate, not yet an
approved rail solution. Its [manufacturer datasheet](https://www.ti.com/lit/ds/symlink/tps65131-q1.pdf)
requires conversion-ratio, switch-current, inductor, diode, ripple and
thermal calculations. Switch-current rating is not output-current rating.
A 150 mA post-regulator must not be selected for the whole headphone rail:
the baseline stereo output can demand approximately 354 mA instantaneous
load current from a rail before amplifier bias and other loads. Check
average demand and capacitor-supported transients separately.

Keep low-noise DAC/I/V supplies separate from high-current output and
speaker boost returns. Follow DAC sequencing, reference bypassing and the
actual analog-stage reference, then verify conducted noise with radio,
display high-voltage refresh, frontlight PWM and USB charging active.
Audio rails are fully disabled in reader-only mode; prove that clocks,
control lines and jacks cannot back-power them.

## AUD-006: Speakers, jacks and fault behavior

Use two TAS2563 devices as the initial stereo speaker candidate. The
[TAS2563 manufacturer data](https://www.ti.com/product/TAS2563) describes
an integrated boost, DSP and I/V sensing; its maximum demonstration output
is not the safe power rating of an unknown small speaker. Initial 2 x 1 W
into nominal 8 Ohm is an engineering ceiling pending transducer selection,
not permission to drive a miniature speaker continuously at that level.
Speaker tuning follows the
[TI Smart Amp characterization guidance](https://www.ti.com/lit/an/slaa953/slaa953.pdf).
Obtain the tuning tools, correct speaker model, thermal/excursion limits
and production coefficients before claiming speaker protection.

3.5 mm uses left, right and a real ground return. Balanced 4.4 mm carries
four independently driven audio conductors; its shell/shield contact is
not either channel's negative output. Never join the negative outputs or
connect a passive common-ground adapter. Exact jack contact numbering and
insertion sequencing must come from the purchased connector drawing.
Use connectors with usable plug detection or separately qualified sensing;
no exact jack MPN is approved by this document.

Provide hardware-default mute, output isolation as required, DC-fault
detection and a safe state when one rail is absent. Characterize shorts
between all audio contacts during insertion, a short to ground, ESD,
repeated hot-plug, cable capacitance, frozen firmware and hard power-off.
No GPIO reset default may energize the speakers or select high gain.
Amplifier thermal shutdown is not a substitute for DC protection or safe
hearing-level defaults. Limit volume on new insertion and after faults;
restore gain/volume only under an explicit policy. Do not measure maximum
power on a person's headphones or ears: use appropriate dummy loads and
an audio analyzer first.

## AUD-007: Initial procurement candidates

Public pages checked 2026-09-07; USD excluding tax/shipping. Stock is a
snapshot, not a reservation. Quantities here are architectural quantities,
not final KiCad references. Complete the native BOM as circuits are placed.

| Role | Exact MPN / source part | Stock | Unit at 1 / 10 / 100 unless stated |
| --- | --- | ---: | --- |
| DAC, 1 | [ES9039Q2M / Mouser 460-ES9039Q2M](https://www.mouser.com/ProductDetail/ESS-Technology/ES9039Q2M?qs=OcgtsXO%2B3gsGISPYpHXYiA%3D%3D) | 547 | $18.70 / $16.13 at 5 / $14.03 at 100 |
| Balanced parallel stage, 4 | [INA1620RTWR / DigiKey 296-51127-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/INA1620RTWR/9555498) | 3,509 | $8.79 / $6.81 / $5.7698 |
| Single-ended stage, 1 | [OPA1622IDRCR / DigiKey 296-45038-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/OPA1622IDRCR/5804204) | 4,615 | $7.43 / $5.734 / $4.8432 |
| DAC I/V/filter, quantity per reference | [OPA1612AIDR / DigiKey 296-39098-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/OPA1612AIDR/2232385) | 576 | $7.40 / $5.709 / $4.822 |
| Speaker amp, 2, QFN candidate | [TAS2563RPPT / DigiKey 296-TAS2563RPPTCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/TAS2563RPPT/14004467) | 581 | $4.37 / $3.314 / $2.7571 |
| Bipolar rail evaluation candidate, 1 | [TPS65131WTRGERQ1 / DigiKey 296-TPS65131WTRGERQ1CT-ND](https://www.digikey.com/en/products/detail/texas-instruments/TPS65131WTRGERQ1/25324547) | 2,253 | $3.25 / $2.434 / $2.0074 |

TAS2563RPPR was not in stock at this check; expected incoming inventory is
not available inventory. TAS2563YBGR is a sourceable alternate package,
not a footprint-compatible substitution. Refresh sourcing before BOM
approval and prefer active manufacturer status over a distributor merely
having old stock. Oscillators, jacks, switches/protection, rail passives and
filter passives remain genuine selection work, not generic placeholders
that can be considered a completed purchasable design.

## AUD-008: Reproducible arithmetic

The standard-library Python block below was executed. It verifies the
stated mathematics, not SPICE behavior, thermal limits or hardware fidelity.

```python
from math import isclose, log10, pi, sqrt

cases = ((16, 0.25), (32, 0.5), (32, 1), (300, 64/300), (600, 64/600))
for load_ohm, power_w in cases:
    vrms = sqrt(load_ohm * power_w)
    irms = vrms / load_ohm
    ipeak = sqrt(2) * irms
    leg_peak = vrms / sqrt(2)
    assert isclose(vrms**2 / load_ohm, power_w)
    assert isclose(2 * leg_peak / sqrt(2), vrms)
    print("load/power/Vrms/Ipeak/each_amp/leg_peak", load_ohm, power_w,
          vrms, ipeak, ipeak/2, leg_peak)

assert isclose(sqrt(2) * sqrt(1/32), 0.25)
for power_w, rail_v, expected_battery in (
    (0.5, 5, 2.892695047520901),
    (1.0, 6, 4.787433687300575),
    (1.0, 8, 6.383244916400766),
):
    ipeak = sqrt(2*power_w/32)
    p_rails = 2*4*rail_v*ipeak/pi + 8*0.0026*2*rail_v
    p_battery = p_rails/0.85
    assert isclose(p_battery, expected_battery)
    print("rail/battery/heat W", p_rails, p_battery, p_battery-2*power_w)

host_input_w = 3.3*0.89627/0.9
audio_input_w = 2.892695047520901
assert isclose(host_input_w+audio_input_w, 6.179018380854234)
print("example whole-source A at 3V", (host_input_w+audio_input_w)/3)
for load_ohm, source_ohm in ((16, 0.5), (32, 0.5), (16, 1.0)):
    print("divider attenuation dB", 20*log10(load_ohm/(load_ohm+source_ohm)))
noise_spl_example = 115 + 10*log10((1e-6)**2/16/0.001)
assert isclose(noise_spl_example, 12.95880017344075)
noise_allocation = sqrt((0.25*0.7e-6)**2+(0.4e-6)**2)
assert noise_allocation < 0.437e-6
print("illustrative IEM noise SPL / allocated V", noise_spl_example, noise_allocation)
low_level_noise_ratio_db = 20*log10(1e-6/sqrt(0.001*32))
assert isclose(low_level_noise_ratio_db, -105.05149978319906)
print("1mW/32Ohm, 1uV noise-to-signal dB", low_level_noise_ratio_db)

bclk_limit = 1/(80e-9)
assert 192000*64 <= bclk_limit < 384000*64
stream_bytes_per_s = 192000*2*4
assert stream_bytes_per_s == 1536000
assert stream_bytes_per_s/1000 > 1023
assert stream_bytes_per_s/8000 == 192
timing_remainder_ns = 1e9/(2*192000*64)-20-4.1
assert isclose(timing_remainder_ns, 16.590104166666664)
print("conditional I2S setup remainder ns", timing_remainder_ns)
print("AUD-003..005 arithmetic PASS; hardware qualification remains separate.")
```

## AUD-009: Native-sheet annotation and release evidence

Use a separate hierarchical audio page or focused DAC/clock, headphone and
speaker subpages. Use real supply symbols and explicit open-drain types;
power flags only identify actual sources for ERC. Keep differential pairs
named by channel and polarity, never as ambiguous shared negative rails.
Connect engineering identifiers to concise notes beside the relevant stage:

```text
AUD-003 / design/audio_subsystem.md
32 Ohm, 0.5 W/ch -> 4 Vrms; Ipeak = sqrt(2*0.5/32) = 176.8 mA.
Parallel pair: 88.4 mA peak per amp before mismatch; qualify hot limit.
4.4 mm negative outputs are driven: never connect to GND or each other.

AUD-004 / design/audio_subsystem.md
RA8P1 SSIE: tBCLK >=80 ns. 192k*64=12.288 MHz; 384 kHz excluded.

AUD-005 / design/audio_subsystem.md
0.5 W/ch, +/-5 V screen: 2.459 W rails / 0.85 = 2.893 W source.
This excludes DAC/filter/clock and does not close the system budget.
```

Approval requires exact I/V/filter and gain values, noise integration,
clock timing, rail conversion/current/thermal calculations, jack/protection
connections, DSP coefficient evidence, whole-product power allocation,
pop/DC/short tests, loaded audio measurements and a completed sourced BOM.
ERC and a readable PDF are necessary schematic checks, not substitutes
for these analog and power validations. Track implementation and unresolved
decisions in issue #840 rather than declaring this engineering basis a
finished audio circuit.
