# poeg_safe_shutoff

Demonstrates the POEG (Port Output Enable for GPT) safe-shutoff path: kill a
running GPT PWM output on a fault trigger, then clear the fault and re-enable
it. This is the mechanism a motor-control H-bridge relies on so an over-current
trip forces the PWM pins high-impedance in a single cycle with no CPU in the
loop.

Each cycle runs the full sequence and verifies every step by reading the state
flag back: GPT0 counting in saw-wave PWM with `POEGG.ST` clear and the outputs
driven; then the software output-disable request (`POEGG.SSF = 1`) latching
`POEGG.ST` and taking the outputs high-impedance; then clearing the request so
that with none left `POEGG.ST` returns to 0 and the outputs drive again.

**POEG gates the output pins, not the counter.** `GTCNT` keeps advancing across
the shutoff, and the demo samples it either side of the hold to show that -- if
the counter stopped too, something other than POEG did it.

POEG can raise the output-disable request from four sources (HUM Ch 21): the
external `GTETRG` pin fed by an over-current comparator, a GPT output-level
short, an oscillation stop, and the software `SSF` bit. This demo arms the
external-pin path so a comparator wired to the POEG pin would trip the shutoff,
and drives the software trigger so the cycle is observable without one. A real
board would additionally route GPT0's output-disable request to this POEG group.

Bare EK-RA8D2; no shields or external transceivers.
