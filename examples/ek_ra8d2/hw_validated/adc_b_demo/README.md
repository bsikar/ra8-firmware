# adc_b_demo

Samples the ADC_B AN000 channel in 12-bit software-triggered mode and
logs each conversion over SCI8 (PD_02 / PD_03 -> J-Link OB CDC). Drives
LED1 as a heartbeat between samples.

Build:

```
make adc_b_demo
```
