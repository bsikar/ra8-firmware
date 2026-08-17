# mem_subsystem

Drives each layer of the `ra8_mem` hierarchy (#147) in isolation, so the
primitives the e-reader leans on are observable without the whole reader stack
around them (#263).

- **`ra8_slab`** -- carve a pool into fixed cells, allocate to exhaustion (the
  next allocation must fail with `no_mem`), free a few, then re-init and confirm
  every cell is back on the freelist.
- **`ra8_arena`** -- bump two aligned sub-blocks out of one region, prove an
  over-subscribed carve fails, then re-init and confirm the whole region is
  available again.
- **`ra8_tile_cache`** -- fault more distinct tiles than the cache has cells to
  force LRU eviction, then read the hit / miss / eviction counters.
- **`ra8_vmem_stream`** -- front a 1 MiB synthetic backing object with a
  four-frame page cache and read byte windows back, crossing frame boundaries,
  clamped at EOF and past EOF, byte-verifying each. The point is that 2 KiB
  resident serves a 1 MiB object: paging, not residency.

Every value in the verdict is computed by the deterministic `ra8_mem` library
with **no MMIO at all**, so the same banner is produced byte-identically by the
host unit test, an emulator and silicon. That makes it a real equivalence check
rather than a boot test, and it means a divergence here is a genuine finding
rather than a modelling gap. Any layer that misbehaves reports which one and
traps on a `BKPT` before reaching the pass line.

No card and no external hardware -- a stock EVM UART is the whole rig.
