# dac_b_demo

Sweeps DAC_B channel 0 in 12-bit normal-vref mode from 0 V up to ~3.3 V
and back, stepping every 20 ms. Distinct from `dac_waveform`, which
plays a sine LUT — this demo exercises the runtime
`ra_dac_b_init_configured` + `ra_dac_b_write` path with a deterministic
DC sweep.

Build:

```
make dac_b_demo
```
