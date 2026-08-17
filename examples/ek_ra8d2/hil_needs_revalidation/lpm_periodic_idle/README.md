# lpm_periodic_idle

The shape a real periodic application takes (#153): each period the CPU does a
small unit of work, then sleeps in Software Standby until the on-chip
Ultra-Low-Power Timer (ULPT0) wakes it, for a fixed number of periods. It reuses
the wake path from `lpm_ulpt_standby` verbatim rather than inventing a second
one.

The pattern is the content. A deep-idle retrofit needs a genuinely armed wake
source plus a per-period marker that survives clock gating -- not a
`while (1) { wfi }` with no wake, which passes in an emulator and hangs on
hardware.

## A Software-Standby wake needs both halves of the ICU

1. **IELSRn + NVIC.** `ra8_isr_register(k_ra8_elc_event_ulpt0_ulpti, ...)` links
   the ULPT0 underflow event (`ULPT0_ULPTI` = `0x080`, HUM Table 19.3 p 823)
   into an ICU IELSRn slot and enables the matching NVIC line. HUM 11.6.2.1
   p 482: the "corresponding IELSRn register must be set before executing a WFI
   instruction"; Table 11.3 footnote 28 p 434: the interrupt "must be enabled by
   NVIC_ISERn".
2. **WUPEN1.ULP0U.** `ra8_lpm_arm_wupen1_bits` arms the asynchronous
   standby-cancel detector (HUM Ch 14.2.20 p 552). `ULPT0_ULPTI` is a valid
   Software-Standby cancel source per Table 11.4 p 434.

Two more HUM-required details, both handled here: confirm the count actually
started (`ULPTCR.TCSTF = 1`, HUM 25.4.7 p 1214) before entering standby, or the
transition can stall the counter before it begins; and keep LOCO running
(`LOCOCR.LCSTP = 0`) so ULPTLCLK survives standby (Table 11.3 footnote 2 p 433).
The wake is therefore LOCO-clocked and does not depend on the intermittent
sub-clock crystal.

The genuine self-wake is silicon-only. An emulator that does not model
Software-Standby clock gating fast-forwards the WFI to the next SysTick, so the
periodic loop advances and the run looks clean without ever proving that a
LOCO-clocked underflow cancels standby.

No external hardware required.
