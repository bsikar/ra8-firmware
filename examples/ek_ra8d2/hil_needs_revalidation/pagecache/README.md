# pagecache

On-silicon HIL for the `ra8_reflow` import-time pagination cache
(`ra8_reflow_cache`, issue #117 / the #79 cache). It runs the cache
write/read round-trip against a real microSD volume through `ra8_fs` --
the exact storage path the e-reader uses -- so the serialise / persist /
load / invalidate logic is exercised against real SD timing and the FAT
read/write path, not just a host unit test.

## What it tests

From a fixed chapter laid out with an SD-loaded font:

1. **Round-trip:** `ra8_reflow_layout_chapter` live, then
   `ra8_reflow_cache_serialize` to a blob, `ra8_fs_write_file` it to the
   card, read it back, `ra8_reflow_cache_load` into a fresh engine, and
   re-serialise -- asserting the reloaded blob is **byte-for-byte equal**
   to the live one (the serialised form is the per-page glyph/page data,
   so equal blobs prove the layout was restored exactly).
2. **Invalidation:** load the same blob into an engine initialised at a
   **different font size** and assert `k_ra8_err_invalid_state` -- the key
   mismatch is caught, no stale page is served.
3. **Reset-survival:** `g_pc_hit` reflects whether the cache file already
   existed at boot, so a second boot against a persisted card reports a
   hit (served from the on-disk cache without a fresh layout).

## Gate (memprobe, not console)

Like the sibling SD HIL apps (`sd_font_render`, `epub_open`), this
gates on SWD-readable globals, not the UART banner: an SD app drives the
SCI0 Simple-SPI bus and ra8_emulator folds every SCI channel into one
console line, so a SCI8 banner is interleaved with SPI traffic there.
`g_pc_heartbeat` advances once per ~100 ms and **only** after every
assertion passes; any failure stamps `g_pc_err` and parks in `wfi`
without bumping it.

| Global | Meaning |
|---|---|
| `g_pc_err` | First failing stage (`pc_err_t`); 0 on success |
| `g_pc_hit` | 1 if the cache file existed at boot (reset-survival) |
| `g_pc_crc_match` | 1 if reloaded-blob CRC == live-blob CRC |
| `g_pc_invalidate` | 1 if the alt-size load returned `invalid_state` |
| `g_pc_pages` | Page count from the live layout |
| `g_pc_live_crc` / `g_pc_cache_crc` | The two serialised-blob CRC-32s |
| `g_pc_heartbeat` | Idle heartbeat; advances only on the clean path |

## Build + run on the M85 simulator (no hardware)

```sh
make pagecache
# Round-trip + invalidation on a blank card (write is slow under the sim,
# so raise the wall/chunk budget):
RA8_EMU_WALL_S=550 RA8_EMU_MAX_CHUNKS=8000000 \
  tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hil_needs_revalidation/pagecache/build/pagecache.elf \
  --sd-new 64:fat32 --save-sd /tmp/pc.img --dump-sym g_pc_crc_match \
  --dump-sym g_pc_invalidate --dump-sym g_pc_err --dump-sym g_pc_heartbeat
# Reset-survival: a second boot against the persisted image -> g_pc_hit=1:
tools/ra8_emulator/build/ra8_emulator ... --sd /tmp/pc.img --dump-sym g_pc_hit
```

## Build + flash (bench)

```sh
make pagecache
make -C examples/pagecache flash        # via on-board J-Link OB
```

Requires a Digilent PMOD MicroSD (410-380) in Pmod2 (J25) with a microSD
inserted. **This app may format the card.** The font and cache file
self-provision, so a blank FAT card works.

## Pass / fail

| Observation | Verdict |
|---|---|
| `g_pc_err==0`, `g_pc_crc_match==1`, `g_pc_invalidate==1`, heartbeat advancing | Cache round-trip + invalidation healthy |
| `g_pc_hit==1` on a second boot of a persisted card | Reset-survival: served from on-disk cache |
| `g_pc_err==6/10` | Serialize / reloaded-CRC mismatch -- the cache did not restore the layout |
| `g_pc_err==11` | Alt-size load did not invalidate (stale-page risk) |
| `g_pc_err==2/3/4` | SD card / mount / font bring-up failed |

## What this does NOT test

- Rendering the cached pages to pixels (that is `sd_font_render` /
  `ereader_ui`); this is the cache serialise/load layer only.
- Cache eviction or multi-book keying beyond the single fixture.
