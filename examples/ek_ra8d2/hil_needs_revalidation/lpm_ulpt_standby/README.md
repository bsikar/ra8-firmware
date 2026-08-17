# lpm_ulpt_standby

Software Standby (`LPSCR.LPMD = 0x5`) woken by the on-chip Ultra-Low-Power Timer
(ULPT0) -- the deep-idle foundation: an MCU that spends nearly all its time in
standby and is woken by an internal periodic timer alone.

The wake source is what makes this app different from the RTC standby demos.
ULPT0 counts down on ULPTLCLK, the LOCO-derived 32.768 kHz clock (HUM Table 9.2
p 320), which keeps running in Software Standby while `LOCOCR.LCSTP = 0` (HUM
Table 11.3 footnote 2 p 433). LOCO is fully on-chip, so this wake does not
depend on the intermittent sub-clock crystal the RTC demos rely on, and the wake
banner is a real signal rather than a boot-only proxy.

## A Software-Standby wake needs both halves of the ICU

Arming the WUPEN bit alone is not enough -- an earlier attempt did exactly that
and never woke.

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
transition can stall the counter before it begins; and keep LOCO running so
ULPTLCLK survives standby.

## It leaves the debug port gated

When the run ends the chip is sitting in a deep LPM state that gates the AHB-AP,
so a probe cannot simply reconnect and flash the next image -- the harness has
to issue an erase-chip first. That is a property of the state, not of this app,
and it is the reason an automated lane that runs this app has to be sequenced
carefully.

No external hardware required.
