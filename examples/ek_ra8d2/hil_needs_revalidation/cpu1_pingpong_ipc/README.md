# cpu1_pingpong_ipc

Dual-core IPC ping-pong across the full TrustZone secure-boot scaffolding
(#22). CPU0 (M85) runs Secure only long enough to programme the SAU, unlock
`PRCR_S.PRC4`, write `IPCSAR = 0x00050000` and `BLXNS` into the Non-Secure
image; the steady-state ping/pong loop runs entirely in NS.

CPU1 (M33) is permanently Non-Secure on this chip -- SECEXT is disabled, per HUM
Ch 2 -- so that IPCSAR write is what makes channels 0 and 2 NS-accessible, and
therefore what lets CPU1 read RXD on channel 2 and write TXD on channel 0.

## Two constraints that bite

**A wrong SAU partition hard-locks the DAP.** The NSC alias regions deliberately
point at the unused `0x10000000` and `0x12000000` IDAU ranges, NOT at the real
`.gnu.sgstubs` location inside lower MRAM. Getting this wrong has bricked a
board here before, and recovery needs the HIL recovery script. Do not flash a
changed SAU layout blind.

**The CPU1 SRAM bank must sit inside physical SRAM.** `SRAM_CPU1` once pointed at
`0x223F0000`, which is reserved -- the on-chip ECC SRAM ends at `0x221A0000`. It
now lives at `0x22190000`, the top 64 KiB of real SRAM. Because that bank is
inside the NS SRAM window, the M33 SAU bring-up needs no separate dedicated-bank
region.

Two globals separate the failure modes. `g_cpu1_pingpong_ipc_tz_step` reaching
`k_ra8_tz_secure_boot_step_branched` says the BLXNS landed; if it stalls short
of that with IPCSAR holding its value, the BLXNS is wedging and SFSR in the
SecureFault handler says why. `g_cpu1_pingpong_match` advancing says the IPC
channels are actually draining; if it sits at zero after a good branch, the open
question is whether `IPCSAR` alone controls RXD reads from NS context or whether
`IPCPAR` has to be configured too.
