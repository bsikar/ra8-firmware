<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# io_blockdev_cache_demo

Maintained consumer for the `ra8_io` caching block device, the write-through LRU
sector-cache decorator declared in `libs/ra8_io/inc/ra8_io_blockdev_cache.h`.
It closes a gap: the
cache compiles into every image that links `ra8_io`, but its only non-test
caller was the parked `hil_needs_revalidation/ra8_io_cache_demo`.

This is not a port of that demo. The parked one reaches the cache indirectly
through FAT12 and VFS traffic and asserts only `hits != 0`. Here there is no
filesystem in the path: every access is a single known LBA against a
deterministic RAM disk, so the hit/miss counters are exact and the eviction
order is provable.

## What it asserts

- `bind`: `ra8_io_blockdev_cache_init` binds, the counters start at zero, and
  `get_caps` forwards the wrapped geometry field by field.
- `hitmiss`: a cold read is exactly one miss and the re-read exactly one hit,
  both delivering the medium's bytes.
- `through`: the write lands in the backing array before the call returns,
  checked behind the device rather than through it, and leaves the block cached.
- `lru`: the pattern 10,11,12,13 then 10,14,12,13,10,11 evicts 11 and keeps 10,
  giving exactly 4 hits / 6 misses. Slot-order eviction would give 3 / 7, so the
  pair pins the policy rather than the data.
- `erase`: an erase drops only the slots covering the range; a cached neighbour
  just outside it still hits.
- `span`: a three-block request is three misses and the repeat three hits, with
  the whole span byte compared both times.
- `guards`: five NULL arguments and a zero slot count to `_init`, plus a NULL
  state to `_stats`, each returning its documented code.

## Geometry

- medium: 64-block (32 KiB) RAM disk in `.bss`, seeded with a pattern byte that
  depends on both the LBA and the byte offset, so a read served from the wrong
  slot or the wrong offset cannot compare equal;
- cache: 4 slots (2 KiB), small enough that eviction is reachable in six reads.

Legs that assert exact totals rebind the cache first, so the counters they check
belong to that leg alone.

## Build and run

```sh
cmake --preset ra8d2-debug
ninja -C cmake-build-debug io_blockdev_cache_demo.elf
```

Nothing outside the SoC is needed: the medium is RAM and the only peripheral is
the SCI8 console, which the ra8_emulator captures. A pass prints

```
io_blockdev_cache_demo: legs=7 hits=0 misses=0 PASS
```

(the trailing counters are the guard leg's freshly rebound cache, so zero is the
expected value there). A failure prints `err=<code> FAIL` with the `ra8_err_t`
of the first leg that broke, and the run parks either way.

## Bench state

`hw_pending`: compiled but not yet run on an EK-RA8D2, so there is no
`hil.conf`. Promote it to `hw_validated/hil` once a bench run shows the PASS
line.
