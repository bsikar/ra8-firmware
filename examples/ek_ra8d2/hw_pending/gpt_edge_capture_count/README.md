# gpt_edge_capture_count

Demonstrates the GPT **hardware input-capture** (issue #185) and
**external event / pulse counting** (issue #186) driver features on the
EK-RA8D2. Unlike `gpt_capture_input`, which approximates capture in
software by polling SW1, this app drives the real edge-latching
hardware:

- **GPT0 -- input capture (#185)**: free-running 32-bit counter;
  `ra8_gpt_capture_configure` arms GTICASR so each rising edge on GTIOC0A
  latches GTCNT into GTCCRA. The loop reads the latch with
  `ra8_gpt_capture_read` and reports the tick delta between edges (the
  measured signal period).
- **GPT1 -- event counting (#186)**: `ra8_gpt_event_count_configure`
  sets GTUPSR so GTCNT increments once per GTIOC1A rising edge (a pure
  external-pulse counter). The commented quadrature variant routes the
  falling edge to GTDNSR for encoder up/down counting.

Build:

```
make gpt_edge_capture_count
```

## Required hardware (why this is hw_pending)

The capture / count inputs need a real edge source -- one of:

1. an **external square-wave signal generator** on the GTIOC0A and
   GTIOC1A pads, or
2. a **jumper loop-back**: run `gpt_pwm_demo` (or any GPT PWM output) on
   a spare channel and wire its GTIOCnA output pin to these capture
   inputs, or
3. a **quadrature encoder** for the up/down (GTUPSR + GTDNSR) variant.

The stock EK-RA8D2 additionally has no cleanly-broken-out GTIOCnA pad --
the candidate pins collide with the debug UART per the Hardware User's
Manual Ch 20.6 pin tables (the same limitation that keeps `motor_3phase`
in `_unsupported/`). The pin assignments in `main.c`
(`k_gpt_ecc_pin_capture` / `k_gpt_ecc_pin_count`) are illustrative
placeholders carrying a `TODO(board-rev)`; update them for a carrier
board or jumper wiring before bench validation.

## HIL plan

Once an edge source is wired, `jlink_memprobe` can assert the published
globals against a known stimulus frequency:

- `g_gpt_ecc_last_period`    -- last valid capture delta (ticks).
- `g_gpt_ecc_capture_events` -- count of valid capture edges.
- `g_gpt_ecc_pulse_count`    -- accumulated GTUPSR pulse count.
- `g_gpt_ecc_tick`           -- pin-independent free-run liveness.

Until that stimulus wiring exists this app is not bench-validated, so it
lives under `hw_pending/`.

## HUM references

- Ch 22.2.10 "GTICASR" p 906-908 / Ch 22.2.11 "GTICBSR" p 909-912 --
  input-capture source select.
- Ch 22.2.8 "GTUPSR" p 898-901 / Ch 22.2.9 "GTDNSR" p 902-905 --
  up / down event-count source select.
- Ch 22.2.20 "GTCCRk" p 938 -- latched capture value.
