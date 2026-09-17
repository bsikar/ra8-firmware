# cache_hal_enable_demo

Boots with the Cortex-M85 L1 I-cache and D-cache brought up through the
`ra8_cache` HAL and proves the core still runs correctly with them on. It is the
HAL-driven twin of `cache_mpu_hil` (#577): same boot posture, same cacheable
round-trip, but the caches are enabled by `ra8_cache_icache_enable()` /
`ra8_cache_dcache_enable()` rather than the shared boot's hand-rolled register
pokes.

The app opts in by defining both `RA8_BOOT_ENABLE_CACHE_MPU` and
`RA8_BOOT_CACHE_VIA_HAL`, so the shared `SystemInit()` programs the MPU, then
takes the HAL route, then enables the branch predictor. The raw-poke helpers
stay compiled in as the untouched reference path -- only an opt-in app diverges.
Each `*_enable` runs its architectural invalidate (ICIALLU for the I-cache, a
set/way invalidate for the D-cache) *before* setting the CCR bit, which is what
makes it safe from a cold cache at boot.

The verdict step fills a `.bss` buffer in M85-private SRAM, clean-invalidates it
back with `ra8_cache_dcache_clean_invalidate_by_addr()`, and reads it back
byte-for-byte, exercising the write-back plus refill path.

## Blocked on

`ra8_emulator` models memory byte-exact and does **not** model the L1 D-cache,
so the cacheable round-trip passes trivially off-target. The hazard this app
guards against exists only on the chip, which makes the bench run the only one
that means anything.
