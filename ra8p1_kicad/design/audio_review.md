# Independent audio architecture review

2026-09-07. Read-only circuit review of `audio_subsystem.md`; this record
does not qualify an implemented schematic, PCB, or measured audio output.
No fatal architecture contradiction was found. The following remain gates
before implementation approval.

## Must close: electrical design

- Close both setup and hold timing for DAC-master/RA8P1-slave I2S at
  192 kHz. The RA 20 ns transmit-delay maximum requires the relevant supply
  to be at least 2.7 V; the lower-voltage maximum is 25 ns. The current
  16.59 ns remainder assumes 50 percent duty cycle and excludes clock load,
  skew and jitter. Include LRCLK timing and the selected pin group.
- Represent ESS clock-pin directions for the selected mode. Register 57
  PCM_MASTER_MODE makes DATA_CLK pin 12 and DATA1 pin 13 BCLK and WS
  outputs despite their input descriptions in the generic pin table.
  Confirm reset and clock-mode transitions cannot create output contention.
- Select a hardware-default-open headphone disconnect and independent
  DC/rail-fault response. Account for both single-ended signal conductors
  and all four balanced conductors. Include disconnect resistance in the
  output-impedance budget; check insertion shorts, asymmetric rail loss,
  powered-off loading and firmware failure. Muting alone is not isolation.
- Complete the I/V/filter, differential and single-ended signal paths,
  gain values, supply conversion, current sharing and thermal design.
  The TI parallel-amplifier example does not qualify a different BTL stage.

Sources: [RA8P1 Hardware User's Manual, Table 70.75](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
[ESS ES9039Q2M v0.2.2, registers 57-58](https://www.mouser.com/datasheet/3/3763/1/ES9039Q2M_Datasheet_v0.2.2.pdf),
[TI parallel INA1620 reference](https://www.ti.com/lit/pdf/sboa352).

## Must close: firmware and hardware contracts

- Define 192 kHz USB playback to 48 kHz speaker-mode behavior: software
  sample-rate conversion or a host-negotiated rate change. Changing the
  DAC clock alone while preserving the old USB stream contract is invalid.
- TAS2563 hardware shutdown loses register state and disables I2C.
  Reload and validate tuning coefficients, gain limits and channel/slot
  selection before each speaker unmute; SDZ high alone is insufficient.
- Select the actual sample-consumption measurement and USB time reference,
  feedback algorithm, bounded buffers, endpoint sizes and clock-change
  sequence. Test suspend, resume, stream stop and oscillator-family changes.
  Keep one coherent logical clock source and explicit feedback for Windows.
- Interlock output selection, gain changes and volume restoration with
  hardware mute/disconnect. Characterize the selected speakers and load the
  matching protection coefficients before claiming speaker protection.

Sources: [TAS2563 datasheet, hardware shutdown and supply sequencing](https://www.ti.com/lit/gpn/tas2563),
[Microsoft USB Audio 2.0 driver requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers),
[USB Audio 2 specification with errata](https://www.usb.org/sites/default/files/Audio2_with_Errata_and_ECN_through_Apr_2_2025.pdf).

## Checks that pass at architecture level

12.288 MHz BCLK satisfies the RA8P1 80 ns minimum period. ESS supports
the required master-clock ratios. The parallel-current and rail-power
calculations are correctly presented as conditional engineering screens,
not guaranteed output performance. No KiCad design files were changed by
this review.
