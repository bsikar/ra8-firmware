# adc_diag_tsn_demo

Runs the ADC_B built-in self-diagnosis (modes 1/2/3) and an end-to-end on-die
temperature read once per cycle, reporting each result on the console. The
entry points under test are the safety-relevant analog HAL surface:
`ra8_adc_self_diagnose` (#182), plus `ra8_tsn_read_die_temp_milli_c` and
`ra8_adc_read_internal_channel` (#183).

`ra8_emulator` models neither the SAR self-test (ADEXDR / ADSGDCR.DIAGVAL) nor
the temperature-sensor path (TSCR), so nothing off-target can arbitrate this
app. It is bench-only, and needs no external hardware.
