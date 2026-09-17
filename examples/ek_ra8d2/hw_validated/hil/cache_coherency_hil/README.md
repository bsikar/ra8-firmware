# cache_coherency_hil

Proves the Cortex-M85 and Cortex-M33 stay coherent across shared on-chip SRAM
**with the M85 data cache enabled**, by routing every cross-core hand-off
through the boot's non-cacheable MPU region.

The image is built with `RA8_BOOT_ENABLE_CACHE_MPU`, so the shared boot enables
the MPU, I-cache and D-cache before `main()` runs. The shared message struct is
pinned at `0x22100000`, which the boot maps as Normal non-cacheable, and that is
the whole trick: the hand-off needs no `ra8_cache_dcache_clean_by_addr` or
`..._invalidate_by_addr` calls at all. Placed in cacheable SRAM instead, the M85
would read a stale pong out of its own D-cache, or hide its ping write from the
cacheless M33, and the round-trip would mismatch. The non-cacheable region
removes exactly that hazard and nothing else.

Coherency here is one-sided: the M33 has no data cache, so only the M85's
matters. The emulator models memory byte-exact with no D-cache, so it passes
trivially -- the hazard is real only on silicon.

Each round the M85 writes a ping payload, issues a `DSB` and bumps a sequence
word; the M33 echoes it with a fixed offset and bumps its own; the M85 verifies.
`g_cache_coherency_match` advances per verified round and
`g_cache_coherency_mismatch` on a bounded-wait timeout or a wrong echo.

The loop keeps round-tripping **forever** after the banner, and that is
load-bearing: the gate samples the counter at two halts and requires a delta, so
a counter frozen after a fixed number of rounds reads the same value at both
halts and fails. `cpu1_pingpong` has the same structure for the same reason.

Stock EK-RA8D2, no add-on hardware.
