# lpm_deep_sleep_demo

Cortex-M85 Deep Sleep on a bare EK-RA8D2. LPSCR.LPMD stays at 0 (System
Active, HUM Ch 11.2.20 p 457) but SCR.SLEEPDEEP is asserted before WFI, so the
core power-gates harder than in plain Sleep -- one tier below `lpm_idle_demo`,
which leaves SLEEPDEEP clear.

Two properties of this silicon shape the app, and both are easy to rediscover
the hard way.

**SysTick does not wake the core from Deep Sleep** under the default LPM
configuration. Once the chip enters WFI it stays there until the next
Initialize. A real wake needs a sub-clock source (RTC or AGT) or an external
IRQ pin, which is out of scope here -- so the automated check keys on the
banner emitted just before the LPM entry and proves the firmware got that far
without faulting, not that wake works.

**The SCI8 module clock (PCLKA) is gated** by the default `ra8_lpm_init` config
while SLEEPDEEP is asserted. An earlier prototype printed one line per wake;
the first post-wake TDR write dropped on the floor and subsequent bytes
mis-framed. Hence the shape here: print once, enter Deep Sleep once, then park
in an ordinary blink loop that is not in LPM, so the bench keeps a reliable
halt window.

Two SWD-readable counters split the failure mode. `g_lpm_deep_pre_count` bumps
before the LPM entry and `g_lpm_deep_wake_count` after it, so a broken LPM init
moves the first and leaves the second at zero.

After this app runs the chip sits in a deep-LPM state with the AHB-AP gated and
cannot simply be reflashed: the bench has to Initialize it first, which is why
this app carries a post-run initialize step. No external hardware required.
