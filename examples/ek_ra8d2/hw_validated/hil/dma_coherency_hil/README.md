# dma_coherency_hil

Single-core (Cortex-M85) HIL test that proves the DMA cache-maintenance
pattern -- **clean-before / invalidate-after** -- keeps a DMAC0
mem-to-mem copy coherent with the M85 L1 **D-cache turned ON**.

## What it proves

This is the first app in the tree built with `RA8_BOOT_ENABLE_CACHE_MPU`
(set in `CMakeLists.txt`). With that flag the shared boot
(`libs/ra8_board_ek_ra8d2/boot/system_init.c`) enables the MPU (5
regions) + I-cache + D-cache + branch predictor. Both buffers live in
MPU **region 1** (`0x22000000`, M85-private **cacheable** SRAM), so the
cache hazard is real on silicon.

With the D-cache enabled a DMAC mem-to-mem copy is no longer
automatically coherent with the CPU:

- After the M85 fills `s_src`, the fresh bytes are **dirty in cache**,
  not in SRAM. The DMAC reads SRAM, so it would copy stale data unless
  the source is **cleaned** first.
- The DMAC writes `s_dst` straight to SRAM. The M85 may still hold
  **stale cached lines** for `s_dst`, so a later read would miss the
  DMAC's data unless those lines are **invalidated** first.

The app does, in order:

1. Fill `s_src` (dirty in D-cache); prime `s_dst` with a sentinel.
2. `ra8_cache_dcache_clean_by_addr(s_src, sizeof s_src)` -- clean-before.
3. DMAC0 ch0 block copy `s_src -> s_dst` (32-bit units, increment both,
   software-triggered via `DMREQ.SWREQ`, completion polled on
   `DMSTS.ACT`).
4. `ra8_cache_dcache_invalidate_by_addr(s_dst, sizeof s_dst)` --
   invalidate-after.
5. Verify `s_dst == s_src`, emit the banner, then park in WFI.

Both buffers are 1 KiB, **32-byte aligned** and an exact whole number of
32-byte cache lines, which is what makes the by-address clean/invalidate
operate on exactly the buffer with no partial-line spill.

## Pass / fail

- PASS: `dma_coherency_hil: dma coherent PASS` over the J-Link OB VCOM
  (and the same result over ITM via `ra8_log_info`), LED1 toggles.
- FAIL: `dma_coherency_hil: dma coherent FAIL`, LED2 toggles.

## board_sim

`tools/ra8_emulator` **does** model the DMAC mem-to-mem transfer
(`board_periph_dmac.c` `dmac_copy_units` moves the bytes in emulated
memory on the software trigger), so the **real** `ra8_dmac` path runs in
sim -- there is no CPU-memcpy fallback. board_sim's memory is byte-exact
and it does not model the L1 D-cache, so the clean/invalidate calls are
exercised but have no caching effect and the copy verifies trivially.
The cache hazard is only observable on real silicon. `RA8_SIMULATOR_MODE`
is a host-unit-test define and is **not** set for the ARM cross-build
board_sim runs, so nothing is `#ifdef`-ed out.

Headless sim run:

```sh
make                                   # cross-compile -> build/dma_coherency_hil.elf
BOARD_SIM_WALL_S=15 BOARD_SIM_IDLE_STOP=1 \
  tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/dma_coherency_hil/build/dma_coherency_hil.elf \
  --panel tools/ra8_emulator/panels/ek_ra8d2.toml
```

## Build / flash

```sh
make            # cross-compile
make flash      # JLinkExe load via scripts/dev/flash.sh
```

Bare EK-RA8D2 only -- no shields or external hardware.
