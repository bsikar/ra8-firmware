# gpt_edge_capture_count

Drives the real GPT edge-latching hardware, where `gpt_capture_input`
approximates capture in software by polling SW1:

- **GPT0 -- input capture (#185).** A free-running 32-bit counter;
  `ra8_gpt_capture_configure` arms GTICASR so each rising edge on GTIOC0A latches
  GTCNT into GTCCRA. The loop reads the latch and reports the tick delta between
  edges, which is the measured signal period.
- **GPT1 -- event counting (#186).** `ra8_gpt_event_count_configure` sets GTUPSR
  so GTCNT increments once per GTIOC1A rising edge -- a pure external-pulse
  counter. A commented variant routes the falling edge to GTDNSR for quadrature
  encoder up/down counting.

## Blocked on

The capture and count inputs need a real edge source that nobody can synthesize
in firmware: an external square-wave generator on the GTIOC0A / GTIOC1A pads, a
jumper loop-back from a GPT PWM output on a spare channel, or a quadrature
encoder for the up/down variant.

The stock EK-RA8D2 additionally has **no cleanly-broken-out GTIOCnA pad** -- the
candidate pins collide with the debug UART per HUM Ch 20.6 pin tables, the same
limitation that keeps `motor_3phase` in `_unsupported/`. The pin assignments in
`src/main.c` (`k_gpt_ecc_pin_capture` / `k_gpt_ecc_pin_count`) are illustrative
placeholders carrying a `TODO(board-rev)`; update them for a carrier board or
jumper wiring before a bench run.

Once an edge source is wired, a memprobe can assert the published globals against
a known stimulus frequency: `g_gpt_ecc_last_period` (last capture delta in
ticks), `g_gpt_ecc_capture_events`, `g_gpt_ecc_pulse_count`, and
`g_gpt_ecc_tick` (pin-independent free-run liveness).

## HUM references

- Ch 22.2.10 "GTICASR" p 906-908 / Ch 22.2.11 "GTICBSR" p 909-912 --
  input-capture source select.
- Ch 22.2.8 "GTUPSR" p 898-901 / Ch 22.2.9 "GTDNSR" p 902-905 -- up / down
  event-count source select.
- Ch 22.2.20 "GTCCRk" p 938 -- latched capture value.
