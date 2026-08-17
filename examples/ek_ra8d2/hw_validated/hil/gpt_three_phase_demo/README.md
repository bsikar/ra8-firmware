# gpt_three_phase_demo

Opens GPT channels 0/1/2 as a phase-synchronised three-phase PWM bank (HUM
Ch 25.2.3, GTSTR / GTBER): all three are armed by one GTSTR write, so they
start on the same PCLKD edge. The duties are offset by roughly a third of the
period each, which is what makes the waveforms motor-control-shaped on a scope.

Without a scope the demo checks the cheaper property -- that the counters
advance and the driver did not reject the multi-channel arm. A failed arm, a
GTSTR that never latched and a gated PCLKD all show up the same way: a counter
that does not move.
