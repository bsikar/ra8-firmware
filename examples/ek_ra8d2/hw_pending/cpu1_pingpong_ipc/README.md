# cpu1_pingpong_ipc (hw_pending)

CPU0 <-> CPU1 ping-pong via the IPC peripheral. Companion to
`cpu1_pingpong` (which uses shared SRAM2 and works today).

## Status: work in progress

Bench-confirmed via the diagnostic experiment shipped in `main.c`:

- `g_cpu1_pingpong_ipc_ipcsar_pre  = 0x00000000` (reset default)
- `g_cpu1_pingpong_ipc_ipcsar_post = 0x00000000` (write was silently dropped)
- CPU0 is therefore in **Non-secure state** at `main()` entry.

This is the root architectural blocker. The IPC channel security
attribution `IPCSAR.SAIPCIRn` (HUM Ch 3.2.1) is one bit per channel
that selects Secure (0, the reset default -- only Secure code can
access) or Non-secure (1 -- both Secure and Non-secure code can
access). `IPCSAR` itself lives in the CPSCU window and is only
writable from Secure state. CPU1 has SECEXT disabled (HUM Ch 2) so
it can only run Non-secure; if CPU0 also runs Non-secure, *neither*
core can write IPCSAR to flip a channel to NS, and the default
S-attributed channels lock both ends out.

## What's needed

To make this app pass HIL, CPU0 must execute at least the IPCSAR
write in Secure state. Two paths:

1. **Boot in Secure via a re-aliased reset vector.** Bench-tested
   in this branch: setting `vector[1] = Reset_Handler + 0x10000000`
   (assuming bit 28 = 1 is the chip's IDAU Secure attribution) and
   relying on SAU-disabled IDAU defaults. Result: **HardFault**.
   RA8D2's IDAU rule is not a simple "bit 28 = Secure" -- HUM Ch 4
   or the FSP `bsp_security.c` references would need a careful read
   to find the correct address alias / SAU primer that puts CPU0 in
   S on reset without bricking.

2. **Full `RA_TRUSTZONE_ENABLE` build.** Mirror the FSP secure-boot
   flow (`R_BSP_SAUInit` + `R_BSP_SecurityInit`) so a real Secure
   image runs first, writes IPCSAR, then `BLXNS` into the
   Non-secure image. The existing `tz_secure_only_*` apps are *not*
   doing this -- their `trustzone_init.c` is intentionally a no-op
   and they rely on the chip's reset defaults instead of running
   real Secure code. So those aren't a usable template here.

## Verification path

Once a Secure-boot path lands and `g_cpu1_pingpong_ipc_ipcsar_post`
reads `0x00050000`:

1. Replace the diagnostic body of `main()` with the ping/pong logic
   that uses `ra_ipc_send_message` / `ra_ipc_recv_message`.
2. Fill `cpu1_main.c` with the matching responder loop.
3. Add a `hil.conf` mirroring `cpu1_pingpong/hil.conf` (memprobe on
   a match counter) and promote the app to `hw_validated/hil/`.

Until then this app stays in `hw_pending/` and serves as the
documented witness that the IPC variant is architecturally possible
but blocked on Secure-boot scaffolding work this repo doesn't have
yet.
