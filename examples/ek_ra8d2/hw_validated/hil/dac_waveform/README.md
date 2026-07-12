# dac_waveform

12-bit DAC_B triangle-wave generator for the bare EK-RA8D2 EVM.

Drives DAC_B channel 0 with a 64-step triangle ramping over the full
0..4095 code range, 1 ms per step. Probe DAC0 with an oscilloscope to
see a ~128 ms (~8 Hz) triangle waveform.

The visible frequency is bound by the millisecond-granularity
`ra8_delay_ms` HAL helper -- once `ra8_delay_us` lands the step period
drops to ~16 us for a 1 kHz waveform.

Build / flash:

```
make dac_waveform
make -C examples/ek_ra8d2/dac_waveform flash
```

## HIL plan

**Requires physical stim -- needs scope on DAC0.** Same situation as
`dac_b_demo`: the only externally-observable signal is the analog
voltage on DAC_B channel 0, and the Pi has no analog input. A
`jlink_memprobe` probe on a step-count global would prove the firmware
called `ra8_dac_b_write` in a loop, but NOT prove the analog pin
actually swung 0..3.3 V on a ~8 Hz triangle.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not yet
visually confirmed the triangle on a scope.
