# dtc_coherency_hil

Proves the DTC descriptor cache-coherency wiring holds with the Cortex-M85 L1
D-cache ON. The app builds with `RA8_BOOT_ENABLE_CACHE_MPU`, so the shared
`SystemInit` programs the MPU and enables the I-cache, D-cache and branch
predictor before `main()`, and the DTC vector table, the Transfer Information
block and both data buffers all live in cacheable M85-private SRAM so the hazard
is real. One software-triggered mem-to-mem block copy, verified, then WFI.

The hazard: the DTC fetches its vector table -- and, one indirection on, each
per-source TI block -- straight from SRAM at activation. With the D-cache on,
the CPU's writes to those descriptors sit dirty in cache where the engine cannot
see them, and a stale fetch sends the transfer at the wrong TI or the wrong
addresses. `ra8_dtc_enable()` cleans the caller-populated vector table back to
RAM before setting `DTCST`. The DTC is direction-blind, like the DMAC, so the
owning driver -- here the app -- cleans the TI block and the source buffer and
invalidates the destination itself.

The ordering is the content:

1. Fill the source and prime the destination with a sentinel, so a no-op copy
   fails the verify instead of passing by accident.
2. Write the TI block and point the vector-table slot at it (both dirty CPU
   writes).
3. Clean source and TI, call `ra8_dtc_enable()` (which cleans the vector table),
   arm `IELSRn.DTCE`, fire the ELC software event.
4. Poll by invalidating the destination and re-verifying each iteration, so the
   read observes RAM rather than the stale cached sentinel. That poll is both
   the completion gate and the coherency proof.

The completion IRQ is deliberately left masked: the registered slot plus `DTCE`
still activate the DTC, and taking the interrupt would let the completion ISR
write `DTCSTS` while the copy is still in flight.

`tools/ra8_emulator` does model the DTC engine and really moves the bytes, so
the whole `ra8_dtc` + ELC path runs off-target -- but its memory is byte-exact
and it models no L1 D-cache, so the poll falls through on its first iteration.
The hazard this app guards against is observable only on silicon.
