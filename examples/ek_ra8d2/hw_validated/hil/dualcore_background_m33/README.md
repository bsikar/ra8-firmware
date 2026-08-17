# dualcore_background_m33

The producer/consumer dual-core pattern: the Cortex-M85 releases the Cortex-M33
and then does nothing but wait while the M33 runs a counting loop entirely on
its own, publishing progress into a shared-SRAM struct. The M85 confirms the M33
booted from a signature field, yields on a bounded poll, and reads the final
count back -- a value only the second core actually executing can produce.
`dualcore_mailbox` is the other shape, where the M85 drives every request/reply
round.

The shared struct is pinned at `0x22100000`, the start of the upper on-chip SRAM
region and below the M33's own bank at `0x22190000`; both linker scripts leave
that address unclaimed, so the same physical bytes back the struct on both
sides. The M33 image is built separately, objcopy'd into a `.cpu1_image` blob
and linked into the M85 ELF, so one flash drops both cores' code into MRAM.
