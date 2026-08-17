# adc_b_demo

Samples ADC_B channel AN000 in 12-bit software-triggered mode, scales the raw
code to millivolts against the board's 3.3 V VREFH, and logs each conversion
over the J-Link OB console. LED1 is the heartbeat between samples.

Nothing on the EVM drives AN000 to a known voltage, so the reading is not ground
truth. What this proves is that a software-triggered conversion starts,
completes, and reads back.
