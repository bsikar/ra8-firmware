# cache_mpu_hil

Boots with the L1 I-cache, D-cache and the five-region MPU enabled
(`RA8_BOOT_ENABLE_CACHE_MPU`) and proves the Cortex-M85 still runs correctly
with them on. Single-core, and the on-silicon arm of the cache-coherency chain.

Because the shared boot programs the MPU and enables the caches before `main()`,
every byte the self-test touches runs with them live:

| Step | MPU region | Memory type | Proves |
|---|---|---|---|
| Cacheable RW | 1 (`0x22000000`, M85-private SRAM) | Normal WB/WA, cacheable | A `.bss` buffer is filled, cleaned and invalidated back to SRAM, then reads back byte-for-byte: the write-back and refill path works. |
| RO MRAM const plus code | 0 (`0x02000000`, MRAM) | RO, execute, cacheable | A non-inlined helper in MRAM `.text` sums a `const` table in MRAM `.rodata` through volatile loads: code executes via the I-cache and the RO region is readable. |
| Device MMIO | 3 (`0x40000000`, peripherals) | Device-nGnRE, uncached | The live SYSTEM `SCKDIVCR` reads back what `ra8_cgc_init()` programmed: peripheral MMIO is mapped Device rather than cached, and is reachable. |

Region 4 -- the shared M85/M33 SRAM bank at `0x22100000`, mapped Normal
non-cacheable -- is deliberately not exercised here. It exists so cross-core
hand-offs stay coherent with no software maintenance, which is a dual-core
property; `cache_coherency_hil` is the app that proves it.

The emulator models memory byte-exact and does not model the L1 D-cache, so the
cacheable-RW step passes trivially off-target. The hazard this app guards
against is only real on the chip, where a missing clean or invalidate, or a
mis-mapped region, corrupts data or faults.

On any mismatch the app emits a distinct FAIL line that never contains the word
PASS, then parks. Every path ends in WFI.
