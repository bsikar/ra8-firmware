# gpt_dma_demo

Sweeps GPT channel 0 PWM duty cycle in saw-wave mode so the LED fades up
and back down at roughly 1 Hz. Demonstrates `ra8_gpt_init` (full
descriptor path) plus runtime `ra8_gpt_set_duty` reconfiguration.

Build:

```
make gpt_dma_demo
```
