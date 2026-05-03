# dac_waveform

12-bit DAC_B triangle-wave generator for the bare EK-RA8D2 EVM.

Drives DAC_B channel 0 with a 64-step triangle ramping over the full
0..4095 code range, 1 ms per step. Probe DAC0 with an oscilloscope to
see a ~128 ms (~8 Hz) triangle waveform.

The visible frequency is bound by the millisecond-granularity
`ra_delay_ms` HAL helper -- once `ra_delay_us` lands the step period
drops to ~16 us for a 1 kHz waveform.

Build / flash:

```
make dac_waveform
make -C examples/ek_ra8d2/dac_waveform flash
```
