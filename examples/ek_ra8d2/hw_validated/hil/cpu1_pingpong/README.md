# cpu1_pingpong

Dual-core round-trip: the M85 releases the M33, writes a ping word into a fixed
shared-SRAM message struct, and the M33 replies with a pong.
`g_cpu1_pingpong_match` advances per verified round-trip and
`g_cpu1_pingpong_mismatch` on a timeout or a wrong word. The counters, not a
banner, are what proves the M33 left reset -- an alive-style check cannot see
that failure at all, because the M85 keeps iterating its outer loop whether or
not the second core ever booted.

## The IPC peripheral is not used, and cannot be

Two facts about this silicon combine into a hard architectural constraint:

- CPU1 is wired with the Security Extension disabled (HUM Ch 2), so the M33 can
  only ever run non-secure.
- IPC channel security attribution (`SAIPCIRn` in IPCSAR) is **mutually
  exclusive** (HUM Ch 3.2.1): a channel is either secure-only or
  non-secure-only, never both. Whichever attribution wins, the other core is
  denied the channel's whole register file.

So no single IPC channel can bridge a secure M85 and a non-secure M33, and a
ping/pong topology built on one is not something firmware can patch around.
IPCSAR is itself secure-only-writable, and a write from a non-secure M85 is
silently dropped with no fault raised, which is what makes this failure mode so
quiet -- the adjacent IPCPAR write in the same register window takes.

The shared-SRAM message struct at `0x22100000` sidesteps the problem entirely
while still proving the M33 was released, booted its vector table, and is
executing its own code. If IPC payloads are ever genuinely needed, the routes
are NSC veneers with the channels kept secure-only, or `IPCSEMn` semaphores used
purely as a readiness signal with the payload still in shared SRAM.

`SRAM_CPU1` has to stay inside physical on-chip SRAM, which ends at
`0x221A0000`. An earlier map placed it at `0x223F0000`, in reserved space, and
the emulator's wide SRAM window hid the mistake completely -- so a memory-map
error of this kind is one only the bench can catch.
