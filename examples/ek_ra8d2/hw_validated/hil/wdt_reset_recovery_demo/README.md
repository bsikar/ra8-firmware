# wdt_reset_recovery_demo

The WWDT (WDT0) half of the watchdog reset story, companion to `watchdog_demo`,
which covers the IWDT.

First boot reads a power-on cause, arms the WWDT with reset-on-expiry, refreshes
it for a couple of seconds, then stops. The WWDT underflows and trips an
internal reset. The second boot reads `k_ra8_reset_cause_wdt0` back out of
`RSTSR1.WDTRF`, prints that it was reset by the watchdog, and parks.

The gate scrapes for the *second* boot's banner, so a pass requires both halves:
the reset has to fire, and the next boot has to decode the cause flag correctly.
That also means the run takes as long as the WWDT underflow countdown, which is
tens of seconds -- the timeout in `hil.conf` is sized for it, not padded.
