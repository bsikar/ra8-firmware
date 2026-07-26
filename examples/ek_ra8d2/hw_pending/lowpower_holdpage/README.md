# lowpower_holdpage -- park the M85, hold the page on the M33

The #150 power-saving model in its simplest demonstrable form: for a static
e-reader page (text shown, waiting for a tap or a page-turn) the Cortex-M85 @
1 GHz is wasted. So the M85 hands the rendered page to the Cortex-M33 @ 250 MHz
and **parks** -- the slow core is plenty to hold the page and service the user
switch, at a fraction of the power. **Power saving = drop to the slow core.**

## What it does

1. **M85 renders page 0.** A real GLCDC render is M85-heavy and out of scope for
   this plumbing demo, so a *page marker* written into the shared mailbox
   (`lowpower_holdpage.h`, at `0x22100000`) stands in for the rendered
   framebuffer. The M85 stamps `active_core = M33` and a `HOLD` magic.
2. **M85 releases the M33 and parks.** `ra8_cpu1_release()` (HUM Ch 2.9.1) starts
   the M33; the M85 then sits in a `wfi` sleep loop -- the low-power posture. It
   only wakes to narrate the M33's heartbeat from the mailbox.
3. **M33 holds the page.** Forever: it lights **LED1 (BLUE, P600)** as the
   "active core = M33" indicator, polls **SW1 (P009)** as the page-turn input
   (on a press it advances the held `page_num` and counts the event), and bumps
   the mailbox heartbeat each loop -- all without waking the M85.

The M85 never drives LED1 or `page_num` after the handoff, so a toggling LED and
a climbing heartbeat are honest proof the M33 is the live core while the M85
sleeps.

## Run it (no hardware)

```
make sim-lowpower_holdpage          # live board view; LED1 toggled by the M33
```
Or headless, holding SW1 to exercise the page-turn:
```
tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_pending/lowpower_holdpage/build/lowpower_holdpage.elf \
  --button 1 --trace --record-secs 2
```
Watch the `GPIO LEDs` summary: LED1 (P600) accumulates transitions driven by the
**M33** (cpu1 engine), proving the held page runs on the secondary core. This
relies on board_sim's cpu1 peripheral modelling (the M33's MMIO routes through
the same peripheral models as the M85).

## What's modelled vs. what's next (#150)

Landed here: the **M85-parks / M33-holds** foundation -- the handoff, the slow-
core hold loop, LED indicator, and SW1 page-turn, all sim-validated.

Still open in #150:
- The real display-plane + framebuffer handoff to the M33 (GLCDC), instead of
  the page marker.
- The **wake path**: the M33 signalling the M85 to spin back up for a heavy
  re-render (opening or compiling a book -- see #149), then handing back and
  re-parking.
- CGC clock-gating of the M85 and the real on-bench battery-delta measurement
  (#30 HIL).

`hw_pending` -- emulator-validated, not yet bench-validated on silicon.
