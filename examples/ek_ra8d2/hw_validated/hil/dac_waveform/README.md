# dac_waveform

Drives DAC_B channel 0 with a triangle spanning the full 12-bit code range, 64
steps at 1 ms each, so one up-and-down period is about 128 ms (roughly 8 Hz).
Probe the DAC0 output pad with an oscilloscope.

That rate is a HAL limit, not a design choice: the step is paced by the
millisecond-granularity `ra8_delay_ms` and there is no microsecond busy-wait
helper in `ra8_time`. With one, the same 64 steps would run at about 16 us for a
1 kHz triangle.

As with `dac_b_demo`, the rig has no analog input, so the unattended gate probes
`g_dac_waveform_tick` over SWD. That shows the write loop running, not that the
pin actually swung.
