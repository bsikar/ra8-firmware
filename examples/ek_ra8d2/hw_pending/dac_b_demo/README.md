# dac_b_demo

Sweeps DAC_B channel 0 in 12-bit normal-vref mode from 0 V up to ~3.3 V
and back, stepping every 20 ms. Distinct from `dac_waveform`, which
plays a sine LUT -- this demo exercises the runtime
`ra_dac_b_init_configured` + `ra_dac_b_write` path with a deterministic
DC sweep.

Build:

```
make dac_b_demo
```

## HIL plan

**Requires physical stim -- needs analog probe on DAC0.** The chip drives
DAC_B channel 0 0..3.3 V; the only externally-observable signal is that
voltage on the DAC pin, plus LED1 toggling. The HIL bench has no ADC /
voltmeter wired to the Pi, so we can't capture the sweep. A weak
`hil_check_alive` (PC-in-MRAM + CycleCnt-advancing) would only prove the
firmware did not HardFault, not that the DAC actually output a sweep.

Relocated from `hw_validated/manual/` on 2026-05-19 -- author has not yet
visually confirmed the waveform on a scope.

To make this HIL-able: instrument main.c with a `volatile uint32_t
g_dac_step_count` advancing every ramp tick, plus `g_dac_write_failures`
that stays at 0; then a `jlink_memprobe` mode would assert step-count
advance + zero failures. That only proves the firmware called the DAC
write API in a loop -- it does NOT prove the analog output actually
moved. Genuine validation needs a scope on DAC0.
