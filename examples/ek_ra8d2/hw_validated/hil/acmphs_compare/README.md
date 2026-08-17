# acmphs_compare

Polls the High-Speed Analog Comparator on channel 0 and routes the decision to
the board LEDs: LED1 toggles while the output reads high, LED2 while it reads
low, LED3 on an init failure. No UART output.

On a bare EVM the IVCMP input floats, so the comparator's decision is not ground
truth -- nothing on the board holds that pin at a known voltage, and LED1 versus
LED2 only proves the chip is alive. The unattended gate therefore probes
`g_acmphs_compare_tick` over SWD, which shows the poll loop running and the
driver not faulting. Asserting the analog decision itself means wiring a known
reference onto IVCMP / IVREF.
