# cpu1_pingpong_ipc (hw_pending)

Dual-core IPC ping-pong demo wired through the full FSP-style
TrustZone secure-boot scaffolding. CPU0 (M85) runs in Secure state
just long enough to programme the SAU, unlock PRCR_S.PRC4, write
IPCSAR = 0x00050000, then BLXNS into the NS image where the
steady-state ping/pong loop runs.

CPU1 (M33) is permanently Non-Secure on this chip (SECEXT disabled
per HUM Ch 2); the IPCSAR write makes channels 0 and 2 NS-accessible
so CPU1 can read RXD on channel 2 and write TXD on channel 0.

> **Memory-map correction (re-validate on bench):** the CPU1 SRAM origin
> (`SRAM_CPU1`) was moved from a **reserved** address (`0x223F0000`, above the
> 1.6 MB on-chip ECC SRAM ending at `0x221A0000`) to `0x22190000`, the top
> 64 KiB of physical SRAM. Because the bank now lives inside the NS SRAM
> window, the M33 SAU bring-up dropped its redundant dedicated-bank region. The
> bench checklist below predates this fix and must be re-run from scratch.

## Status

This app lives in hw_pending/ because **bench validation has not
been performed**. The SAU layout has been chosen carefully to avoid
the brick documented in `memory/project_sau_sgstubs_brick.md` -- the
NSC alias regions point at the unused 0x10000000 and 0x12000000 IDAU
ranges, NOT at the actual `.gnu.sgstubs` location inside lower MRAM.
But TrustZone bring-up on this chip has a documented record of
hard-locking the DAP when the SAU partition is wrong, and recovery
needs `scripts/hil/recover.sh`. **Do not flash blind.**

## What's compile-verified

- Host unit tests cover the SAU region layout (no overlap, NSC
  alias outside Secure MRAM, 32-byte alignment), the PRCR_S unlock
  sequence (PRC4 open -> IPCSAR write -> PRC4 close), and the
  BLXNS transition state capture (MSP_NS + VTOR_NS + reset vector).
- `make tidy` and the pre-commit hook chain (ASCII, no-AI-attribution,
  format, doxy-audit, citation check) pass.
- `make test` (host unit tests) passes.
- Cross-compile builds cleanly with `RA8_TRUSTZONE_ENABLE=ON`.

## What's bench-pending

- Whether the SAU enable actually lands without HardFault on the
  real chip.
- Whether the PRCR_S unlock + IPCSAR write produces the documented
  read-back value 0x00050000.
- Whether the BLXNS jump into the NS image at 0x02080000 reaches
  `main()` cleanly.
- Whether the ping/pong loop round-trips ping=0x1234 / pong=0x4321
  through `ra8_ipc_send_message` / `ra8_ipc_recv_message` on
  channels 0 and 2.

## Bench checklist (human operator)

1. Flash via `scripts/hil/flash.sh` (NOT `scripts/dev/flash.sh`, per the
   project memory note about flashing going through the Pi).
2. Have `scripts/hil/recover.sh` warm in a second terminal.
3. JTAG read `0x40008610` -- IPCSAR must read 0x00050000.
4. JTAG read `g_cpu1_pingpong_ipc_ipcsar_post` -- same value.
5. JTAG read `g_cpu1_pingpong_ipc_tz_step` -- must be 6
   (`k_ra8_tz_secure_boot_step_branched`).
6. JTAG read `g_cpu1_pingpong_match` -- should be advancing.

If step 3 holds but step 5 stays below 6, the BLXNS is wedging --
look at `secure_exception.c`'s `SecureFault_Handler` capture in
SFSR. If step 5 reaches 6 but `g_cpu1_pingpong_match` stays at 0,
the NS image is up but the IPC channels are still not draining --
the next step is to check whether `IPCSAR` bits 16 and 18 actually
control RXD reads from NS context, or whether the chip also needs
`IPCPAR` to be configured.

## See also

- Issue #22 -- the TrustZone scaffolding follow-up to #17.
- `libs/ra8_tz_secure_boot/` -- the shared secure-boot implementation.
- `memory/project_sau_sgstubs_brick.md` -- brick risk.
- `memory/project_ipcsar_prcr.md` -- PRCR_S.PRC4 unlock pattern.
- `memory/project_cpu1_must_be_non_secure.md` -- CPU1 SECEXT topology.
