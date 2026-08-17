# dualcore_mailbox

Two independently compiled images -- one Cortex-M85, one Cortex-M33 -- stitched
into a single flashable image, with both cores running at once and exchanging
messages through a shared-SRAM mailbox. Each round the M85 sends an operand and
the M33 returns `n*3 + 1`. That tiny computation is the point: a correct reply
can only have been produced by the second core executing, so it is honest
evidence of life rather than the M85 reading back its own write.

This is the two-*cores* axis. For the Secure/Non-Secure flavour of "two images"
on a single core, see the TrustZone e-reader application.

- **Release.** `ra8_cpu1_release()` writes the M33's vector-table base to
  `CPU1INITVTOR` and asserts `CPU1ACTCSR.ACTREQ` to bring the second core out of
  power-gating (HUM Ch 2.9.1 "CPU control registers" p 128-130). The M33 then
  fetches its reset vector.
- **Mailbox.** The two cores share no cache but do share on-chip SRAM. The
  mailbox struct is pinned at `0x22100000`, which both linker scripts leave
  unclaimed, so the same physical bytes back it on both sides. The M85 data
  cache is left off, so a `dsb` after each write is all the coherency needed.
- **Image stitching.** The M33 image is objcopy'd into a `.cpu1_image` blob and
  linked into the M85 ELF, pinned at `MRAM_CPU1` (`0x020C0000`), so one SWD
  flash drops both cores' code into MRAM.

Only the M85 narrates. Each core has its own ITM on hardware, but the emulator
wires only the primary core's to the console, so rather than print lines nobody
can see, the M33 stays silent and the M85 reports the replies it received --
values that can only have come from the M33.

A default build is silent because `ra8_log_info` is compiled out below INFO
level. The dual-core exchange still runs; a Debug build is what makes it
visible.
