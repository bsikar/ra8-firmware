# motor_3phase

Runs GPT0/1/2 as a synchronized three-phase PWM triple with a dead-time pair on
each channel, sweeping the duty cycles through a sine table. It is written for
an external gate-driver IC.

Untested, and the pin choice is a placeholder: the EK-RA8D2 header pins for a
motor-driver IC were never confirmed against the board manual, so the GTIOC
outputs in `src/main.c` are guesses and are marked as such. Confirm them before
wiring anything that moves.

The firmware emits only the three high-side gate signals, because a driver IC
generates the complementary low side itself with its own dead-time. For a board
that needs both rails from the MCU instead, the same channels' GTIOCnB pins
carry the inverted signals automatically once a non-zero dead-time is
programmed (HUM Ch 22.5 "Complementary PWM Mode").

The sine table is built at startup from a fixed-point Bhaskara I approximation
rather than by calling `sin()`, which would drag a soft-float implementation
into the image:

```
sin(x) ~= 16 * x * (pi - x) / (5 * pi^2 - 4 * x * (pi - x))    for x in [0, pi]
```

The three counters are started in one write to the group-start register, which
is what makes the phases genuinely synchronized rather than merely
near-simultaneous.

Checked on paper against EK-RA8D2 v1 UM Table 20 p 27 (Arduino GTIOC routings)
and HUM R01UH1065EJ0130 Ch 22 "General PWM Timer (GPT)". Not checked on
silicon.
