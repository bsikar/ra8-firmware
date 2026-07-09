# fault_crashlog_hil

Proof of the **cross-reset crash-log + reset-loop guard** (`ra_crashlog`,
`libs/ra_core`) that sits on top of the fault decoder (T2-01/02): the
decoded fault snapshot is persisted into a `.noinit` record the reset
handler does **not** zero, crash-reboot cycles are counted, and a
safe-mode request latches once the count crosses a threshold.

This is the persistence sibling of `fault_div0_hil`: that app provokes and
**decodes** a real CPU fault; this app **survives** one across a reset.

## What it does

1. Brings up clocks + the SCI8 J-Link VCOM console (115200) and routes
   `ra_log` there so a real fault dump would be visible on the bench UART.
2. `ra_crashlog_install()` -- arms the exception-persist hook, so any later
   real fault is copied into the `.noinit` record before the CPU halts.
3. Peeks the record. A prior record (from a warm/watchdog reset) prints its
   `exc pc boot_loops`; a cold/first boot reads empty. If `boot_loops`
   crossed `k_ra_crashlog_loop_threshold`, `ra_crashlog_safe_mode_requested()`
   latches: the app prints `SAFE-MODE`, `ra_crashlog_claim()`s to reset the
   guard, and idles instead of the risky path.
4. In-process write+decode proof: synthesises a decoded snapshot, pushes it
   through the installed write path (`ra_crashlog_record_fault`), and reads
   it back.

## Expected output (cold / first boot -- board_sim and bench)

```
crashlog: boot
crashlog: no prior record (cold/first boot)
crashlog: recorded+readback exc=6 pc=0x02001234 boot_loops=1
```

The `hil.conf` gate scrapes the `recorded+readback` line (the write+decode
proof) and rejects a `FAIL`.

## What survives which reset class

The `.noinit` record lives in a dedicated `NOINIT` region carved from the
top 256 bytes of SRAM (so the main stack, which ends at the top of SRAM
proper, sits directly below it), and is NOLOAD so `Reset_Handler`'s `.bss`
zero-fill never touches it. It
therefore survives any reset that keeps SRAM **powered**, but not a power
cut:

| Reset class                          | SRAM kept | Record survives |
|--------------------------------------|-----------|-----------------|
| Watchdog / IWDT underflow (warm)     | yes       | yes             |
| Software reset (SYSRESETREQ)         | yes       | yes             |
| Pin / debugger reset (no power cut)  | yes       | yes             |
| Power-on reset / LVD brown-out       | no        | no (fail-safe)  |

A cold power-on randomises SRAM; the record's `magic` + CRC then fail
validation and the log reads empty -- the intended fail-safe (no false
post-mortem from garbage SRAM). **Surviving a full power loss is out of
scope** and is the named follow-up: VBATT-backed / MRAM persistence via
`ra_bkup`, currently silicon-blocked (#131).

## Why `hw_pending`

`tools/board_sim` **cold-loads** the image on every run and its Unicorn
core cannot take a real CPU fault, so it can prove the boot bring-up, the
peek / claim / safe-mode logic, and the synthesised in-process write+read
back -- but **not** the cross-reset survival or the real
fault->hook->record path. Those two legs are proven elsewhere:

- the genuine `ra_exception_report` -> hook -> record path, plus the
  fill / validate / claim / loop-counter / threshold / corrupted-magic
  lifecycle, is proven in-process by `tests/test_ra_crashlog.c`;
- the real cross-reset survival + `boot_loops` climb toward safe mode is a
  **bench-only** leg: reset the board *without cutting power* (e.g. the
  J-Link `reset` / a watchdog underflow) and watch `boot_loops` count up on
  each `crashlog: prior record ...` line until `SAFE-MODE requested`.

Promote to `hw_validated/hil/` once the bench captures that warm-reset
climb:

```
make -C examples/ek_ra8d2/hw_pending/fault_crashlog_hil build flash
# scrape the SCI8 VCOM console; warm-reset (keep power) to see boot_loops climb
```
