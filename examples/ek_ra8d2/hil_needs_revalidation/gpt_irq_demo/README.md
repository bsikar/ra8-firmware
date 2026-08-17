# gpt_irq_demo

Interrupt-driven GPT0: links the GPT0 counter-overflow event through the ICU
IELSR table to an NVIC line with `ra8_isr_register`, starts GPT0 in saw-wave PWM
mode, then sleeps. Every overflow fires an application ISR in NVIC handler mode
that bumps `g_gpt_irq_count` and toggles LED1.

The main loop never touches the LED, so any LED activity -- or any advance in
that counter -- proves the real interrupt path ran end to end, which is what
separates this from the poll-based `gpt_one_shot_demo` and `agt_periodic`. A
frozen counter narrows to the GPT module-stop gate, a wrong IELSR event link, a
disabled NVIC line, or a vector that never reaches the dispatcher.
