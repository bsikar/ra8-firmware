# lpm_ulpt_standby

Software Standby (LPSCR.LPMD = 0x5) woken by the on-chip Ultra-Low-Power
Timer (ULPT0) on the bare EK-RA8D2 EVM.

Software Standby gates almost every clock domain -- including the CPU and
SysTick. The wake here comes from ULPT0 counting down on **ULPTLCLK**, the
LOCO-derived 32.768 kHz clock (HUM Table 9.2, p 320), which keeps running
in Software Standby while `LOCOCR.LCSTP = 0` (HUM Table 11.3 footnote *2,
p 433). Because LOCO is fully on-chip, this wake source does **not** depend
on the intermittent sub-clock crystal (SOSC) that the RTC-based standby
demos rely on -- so unlike them, the wake banner here is a real,
bench-gatable signal.

This is the deep-idle foundation: an MCU that spends almost all of its
time in Software Standby and is woken by an internal periodic timer alone.

## What it does

1. CGC + SysTick + SCI8 + ULPT + LPM block bring-up; keep LOCO running.
2. Wire the ULPT0 underflow as a Software-Standby wake source (both halves
   of the RA8D2 path -- see below).
3. Emit boot banner `lpm_ulpt: boot` over SCI8 (once).
4. Loop:
   - Start ULPT0 counting down from ~0.5 s and confirm `ULPTCR.TCSTF = 1`.
   - `ra8_lpm_enter_sleep(k_ra8_sleep_mode_software_std)`.
   - On the underflow wake, stop ULPT0 (clears `ULPTCR.TUNF`) and print
     `lpm_ulpt: wake`.

## The RA8D2 standby-wake path (what makes this work)

A Software-Standby wake on the RA8D2 needs **both** halves of the ICU
wired -- arming only the WUPEN bit (as an earlier attempt did) is not
enough:

1. **IELSRn + NVIC** -- `ra8_isr_register(k_ra8_elc_event_ulpt0_ulpti, ...)`
   links the ULPT0 underflow event (`ULPT0_ULPTI` = `0x080`, HUM Table
   19.3, p 823) into an ICU IELSRn slot and enables the matching NVIC
   line. HUM 11.6.2.1 (p 482): "corresponding IELSRn register must be set
   before executing a WFI instruction"; Table 11.3 footnote *28 (p 434):
   the interrupt "must be enabled by NVIC_ISERn".
2. **WUPEN1.ULP0U** -- `ra8_lpm_arm_wupen1_bits(k_ra8_lpm_wupen1_ulpt0u)`
   arms the async standby-cancel detector (HUM Ch 14.2.20, p 552).
   `ULPT0_ULPTI` is a valid Software-Standby cancel source per Table 11.4
   (p 434).

Two further HUM-required details, both handled here:

- **Confirm the count started** before entering standby (`ULPTCR.TCSTF = 1`,
  HUM 25.4.7, p 1214) -- otherwise the standby transition can stall the
  counter before it begins.
- **Keep LOCO running** (`LOCOCR.LCSTP = 0`) so ULPTLCLK survives standby.

## HIL gate

`HIL_MODE=uart_scrape` on the banner `lpm_ulpt: wake`. Unlike the RTC
standby demos (which gate on the boot banner only because SOSC is flaky),
this gates on the **actual wake** -- the ULPT runs off LOCO, so the wake
is deterministic. `HIL_POST_INITIALIZE=1`: after the run the chip is in a
deep-LPM state that gates the AHB-AP, so the harness runs an erase-chip
(Initialize) before the next app's flash.

## Bench status

Bench-verified on the EK-RA8D2: ULPT0 underflow repeatedly cancels
Software Standby and the `lpm_ulpt: wake` banner streams at the ~0.5 s
period. No external hardware required.
