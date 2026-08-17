# gpt_pwm_demo

Drives an LED off GPT channel 0 in saw-wave PWM mode, walking the duty cycle
from zero to full and back so it visibly breathes.

Each step also samples GTCNT and only counts the iteration a success if the
counter actually moved since the previous sample. Configuring a timer without
faulting proves nothing: a closed clock gate leaves GTCNT frozen at zero while
every driver call still returns ok. The check is that the peripheral is
counting, not that it is configured. Needs no external hardware.
