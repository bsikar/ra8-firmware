# agt_pulse_demo

Runs AGT0 in pulse-output mode with compare-match A driving the AGTOAn pin
(HUM Ch 24.3.4 "Pulse Output Mode" p 1177), toggling LED1 and logging the tick
count on every underflow.

Each underflow stops and re-arms the channel, which is how `AGTCR.TUNDF` gets
cleared. `agt_periodic` is the same timer with no pin output.
