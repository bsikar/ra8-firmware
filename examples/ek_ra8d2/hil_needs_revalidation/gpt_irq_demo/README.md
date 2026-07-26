# gpt_irq_demo

Interrupt-driven GPT0 demo: links the GPT0 counter-overflow event through the
ICU IELSR table to an NVIC line with `ra8_isr_register`, starts GPT0 in saw-wave
PWM mode, then sleeps. Every overflow fires an application ISR (in NVIC handler
mode) that bumps `g_gpt_irq_count` and toggles LED1. The main loop never touches
the LED, so any LED activity proves the real interrupt path ran -- the
counterpart to the poll-based `gpt_one_shot_demo` / `agt_periodic`.

This is also the timer-interrupt proof target for `tools/ra8_emulator`: the emulator
counts the GPT overflow, pends NVIC IRQ0 through its ICU model, and vectors it in
as a real Cortex-M exception.

Build:

```
make gpt_irq_demo
```
