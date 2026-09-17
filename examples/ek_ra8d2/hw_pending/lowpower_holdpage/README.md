# lowpower_holdpage

The #150 power-saving model in its simplest demonstrable form: for a static
e-reader page -- text shown, waiting for a tap -- the Cortex-M85 @ 1 GHz is
wasted, so it hands the page to the Cortex-M33 @ 250 MHz and parks. Power saving
means dropping to the slow core.

1. **The M85 "renders" page 0.** A real GLCDC render is M85-heavy and out of
   scope for this plumbing demo, so a page marker in the shared mailbox at
   `0x22100000` stands in for the rendered framebuffer.
2. **The M85 releases the M33 and parks.** `ra8_cpu1_release()` (HUM Ch 2.9.1)
   starts the M33; the M85 then sits in a `wfi` sleep loop, waking only to
   narrate the M33's heartbeat out of the mailbox.
3. **The M33 holds the page.** It lights LED1 (P600) as the active-core
   indicator, polls SW1 (P009) as the page-turn input, and bumps the mailbox
   heartbeat each loop -- all without waking the M85.

The M85 never drives LED1 or the page number after the handoff, so a toggling LED
and a climbing heartbeat are honest proof that the M33 is the live core while the
M85 sleeps.

## Blocked on

What is landed here is the handoff, the slow-core hold loop, the LED indicator
and the SW1 page-turn. What no emulator can supply is the real on-bench battery
delta of the park (#30), which is the entire point of the exercise. The real
GLCDC display-plane and framebuffer handoff to the M33, in place of the page
marker, is the other open half of #150.
