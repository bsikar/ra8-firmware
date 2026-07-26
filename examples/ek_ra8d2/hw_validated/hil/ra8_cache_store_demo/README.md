# ra8_cache_store_demo

Exercises `ra8_cache_store` (the persistent key->blob cache from issue #201, the
on-media backing for compiled `.rabook` containers) directly, end to end, on the
EK-RA8D2 -- the example the #252 coverage sweep found missing (#257).

## What this app does

Brings the chip up like `crc_demo` (CGC + SysTick + the J-Link OB VCOM console @
115200 8N1), then runs the whole cache lifecycle once through the hardware-free
`cache_store_demo_run` core:

1. **Mount.** Format + mount a `ra8_cache_store` over LevelX standalone, bound to
   a RAM-backed NOR driver (`src/lx_nor_ram.c`).
2. **Put / get.** Put four keyed "render/glyph cache" blobs (a cover glyph atlas
   plus three render tiles, sizes crossing sector boundaries) and read each back
   byte-identical.
3. **Pin + force an eviction.** Pin the open-book cover atlas (never evict), then
   evict a render tile. Confirm the evicted key is now a miss, the survivors
   still resolve, and evicting the pinned atlas is refused (`busy`).
4. **Reuse.** Re-put a new blob, proving the evicted entry's sectors were
   reclaimed.
5. **Persist across a remount.** Checkpoint-close, then re-mount over the same
   media with a fresh (zeroed) control block -- a simulated reboot -- and confirm
   the survivors (including the pinned atlas) come back byte-identical, the
   evicted key stays gone, and the pin persisted across the checkpoint.

On success it prints the success-only banner

```
[rcs] cache_store demo PASS survivors=4 evicted_gone=1 pin=1
```

then idles, re-emitting it. On any failure it prints
`[rcs] cache_store demo FAIL stage=S status=C` instead.

## Why a RAM-backed NOR driver (RAM block device)

`ra8_cache_store`'s physical-flash bind is an injected callback
(`ra8_cache_store_nor_init_fn`). Production binds the Octo-SPI driver; this demo
binds a RAM driver (`lx_nor_ram_init`) so the entire persistent-cache path runs
in SRAM with **no MMIO**. Two payoffs:

- **Sim-gateable.** issue #257 calls for the RAM-block-device path precisely
  because it needs no external hardware.
- **SIM equals HIL by construction.** Because the cache path touches no
  peripheral register, the board_sim (SIL) run executes byte-identical
  instructions to the on-silicon run -- there is nothing for the emulator to
  model differently from the chip.

The RAM backing persists across `close` / re-`init` **within one boot**, which
models the "reboot: control RAM lost, on-media content survives" behaviour the
persistence demonstration needs. (A real Octo-SPI part persists across an actual
power cycle; the RAM model persists across the in-process remount.)

## Build dependencies

The per-app `Makefile` forces `-DRA8_USE_LEVELX_STANDALONE=ON`, which pulls in
the vendored `libs/third_party/levelx/` NOR sources built with
`LX_STANDALONE_ENABLE` (no ThreadX) via `cmake/levelx_standalone.cmake`. The
store itself is `libs/ra8_cache_store` (`USES levelx_standalone`,
`LIBS ra8_cache_store`).

## Recipe

1. Connect a USB cable to the J-Link OB CDC port on the EK-RA8D2.
2. Open a 115200 8N1 terminal (`picocom -b 115200 /dev/cu.usbmodem...` on macOS,
   `minicom -D /dev/ttyACM0 -b 115200` on Linux).
3. Build and flash:

   ```
   cd examples/ek_ra8d2/hw_validated/hil/ra8_cache_store_demo
   make
   make flash
   ```

4. Watch the PASS banner stream out.

## SIL / HIL gate

`hil.conf` gates the app in `uart_scrape` mode on the success-only PASS banner.
Because the whole path is RAM-resident, `scripts/sim/sil_all.sh` runs it headless in
`tools/ra8_emulator` with no board attached and the emulated run matches the bench.

## Host test

The same `cache_store_demo_run` core and the same RAM NOR driver are compiled
into `tests/test_cache_store_demo.c`, so the host unit test drives byte-identical
logic on the x86_64 host (`make test`). The store's own primitives have full
coverage in `tests/test_ra8_cache_store.c`.
