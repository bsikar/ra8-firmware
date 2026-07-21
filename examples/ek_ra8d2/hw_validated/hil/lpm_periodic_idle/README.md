# lpm_periodic_idle

The "periodic apps" increment of the deep-idle foundation (roadmap issue
#153). It takes the bench-proven ULPT0 self-wake from `lpm_ulpt_standby`
and wraps it into the shape a real periodic application takes: each period
the CPU does a small unit of work, then sleeps in Software Standby until the
on-chip Ultra-Low-Power Timer (ULPT0) wakes it, repeating for a fixed number
of periods before reporting `lpm_periodic_idle PASS` and parking.

This is the clean pattern an idle-loop retrofit should follow: a real armed
wake source (ULPT0 underflow) plus a per-period marker that survives clock
gating -- not a `while(1){ wfi }` with no wake, which sim-passes but
hardware-hangs (see issue #153).

## What it does

1. CGC + SysTick + SCI8 + user LED + ULPT + LPM block bring-up; keep LOCO
   running so ULPTLCLK survives Software Standby.
2. Wire the ULPT0 underflow as a Software-Standby wake source (both halves
   of the RA8D2 path -- see below).
3. Emit boot banner `lpm_periodic_idle: boot` over SCI8 (once).
4. Loop `k_lpi_period_count` (8) periods:
   - **Work**: toggle the blue user LED (heartbeat) and print
     `lpm_periodic_idle: work`.
   - **Arm**: start ULPT0 counting down from ~0.5 s and confirm
     `ULPTCR.TCSTF = 1`.
   - **Standby**: `ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std)`.
   - **Self-wake**: the ULPT0 underflow cancels Software Standby; stop ULPT0
     (clears `ULPTCR.TUNF`).
5. Emit `lpm_periodic_idle PASS` and park.

## The RA8D2 standby-wake path (reused verbatim)

The exact, bench-proven wake path from `lpm_ulpt_standby` is reused -- no new
mechanism is invented. A Software-Standby wake on the RA8D2 needs **both**
halves of the ICU wired:

1. **IELSRn + NVIC** -- `ra8_isr_register(k_ra8_elc_event_ulpt0_ulpti, ...)`
   links the ULPT0 underflow event (`ULPT0_ULPTI` = `0x080`, HUM Table 19.3,
   p 823) into an ICU IELSRn slot and enables the matching NVIC line. HUM
   11.6.2.1 (p 482): "corresponding IELSRn register must be set before
   executing a WFI instruction"; Table 11.3 footnote *28 (p 434): the
   interrupt "must be enabled by NVIC_ISERn".
2. **WUPEN1.ULP0U** -- `ra8_lpm_arm_wupen1_bits(k_ra8_lpm_wupen1_ulpt0u)` arms
   the async standby-cancel detector (HUM Ch 14.2.20, p 552). `ULPT0_ULPTI`
   is a valid Software-Standby cancel source per Table 11.4 (p 434).

Two further HUM-required details, both handled here:

- **Confirm the count started** before entering standby (`ULPTCR.TCSTF = 1`,
  HUM 25.4.7, p 1214) -- otherwise the standby transition can stall the
  counter before it begins.
- **Keep LOCO running** (`LOCOCR.LCSTP = 0`) so ULPTLCLK survives standby
  (HUM Table 11.3 footnote *2, p 433). The wake is therefore LOCO-clocked and
  does not depend on the intermittent sub-clock crystal (SOSC).

## Bench status (hw_pending)

**Not yet bench-validated.** board_sim does not model Software-Standby
clock-gating: it fast-forwards `wfi` to the next SysTick, so the periodic
loop advances in the simulator (the boot banner, all eight `work` banners,
and `PASS` print without fault) but the genuine ULPT0 self-wake -- the
LOCO-clocked underflow cancelling Software Standby -- can only be confirmed
on the EK-RA8D2. This example is correct-by-construction against the
bench-proven `lpm_ulpt_standby`, which validates that exact wake path. Once
run on the board it can graduate to `hw_validated/`.

No external hardware required.
