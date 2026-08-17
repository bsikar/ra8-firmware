# dac_b_demo

Sweeps DAC_B channel 0 in 12-bit normal-vref mode from 0 V up to about 3.3 V and
back, stepping every 20 ms. `dac_waveform` drives a triangle over the same
channel; this one is a deterministic DC ramp, so it exercises the runtime
`ra8_dac_b_init_configured` and `ra8_dac_b_write` path with a predictable value
at every step.

Nothing on the bench reads the analog pin -- the rig has no ADC or voltmeter --
so the unattended gate probes `g_dac_b_demo_tick` over SWD. That shows the
firmware calling the DAC write API in a loop and not faulting; it does **not**
show the pin voltage moving. Genuine validation needs a scope on DAC0.
