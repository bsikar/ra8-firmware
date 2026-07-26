# mem_subsystem

Drives each layer of the `ra8_mem` (#147) memory hierarchy in isolation on the
EK-RA8D2, so the primitives the e-reader leans on are observable without the whole
reader stack. Closes #263 (from the #252 example-coverage sweep).

The `ra8_mem` primitives -- `ra8_slab`, `ra8_arena`, `ra8_tile_cache`, and the
`ra8_vmem` / `ra8_vsource` / `ra8_vmem_stream` page cache -- are exercised inside
the reader but had no direct example. This app runs each one on its own and prints
one deterministic banner of the observables over the board UART.

## What it drives

| Layer | Module | Exercise |
|-------|--------|----------|
| 0 | `ra8_slab` | Carve a 512 B pool into 8 x 64 B cells, allocate all 8 (the 9th `alloc` returns `no_mem`), free 3, then **reset** by re-init and confirm all 8 cells are free again. |
| 0 | `ra8_arena` | Bump two aligned sub-blocks (1000 B @ 8, 2000 B @ 16) out of one 4 KiB region, prove an over-subscribed 2000 B carve fails `no_mem`, then **reset** by re-init and confirm the whole region is available. |
| 3b | `ra8_tile_cache` | Fault 8 distinct tiles through a 4-cell cache to force LRU eviction, then read the hit / miss / eviction counters (`hits=4 misses=9 evictions=5`). |
| 2 | `ra8_vmem_stream` | Front a **1 MiB** synthetic backing object with a fixed **4-frame (2 KiB-resident)** page cache and read back byte windows -- crossing frame boundaries, clamped at EOF, and past EOF -- byte-verifying each and folding them into a CRC-32. Paging, not residency: 2 KiB resident serves a 1 MiB object. |

## Banner

```
mem: slab_free=8 arena_free=4096 tile_evict=5 vmem_crc=D1E67963 PASS
```

logged over SCI8 (115200 8N1, J-Link OB CDC). Every value is computed by the
deterministic `ra8_mem` library with **no MMIO**, so the banner is byte-identical
on the host unit test, board_sim, and silicon. Any layer that misbehaves prints
`mem: FAIL <layer>` and traps on a `BKPT` before `PASS`; `hil.conf`'s negative
regex catches that fast.

```
make mem_subsystem                       # cross-compile -> build/mem_subsystem.elf
tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hil_needs_revalidation/mem_subsystem/build/mem_subsystem.elf
bash scripts/sim/sil_all.sh --only mem_subsystem   # headless board_sim gate
make hil-flash APP=mem_subsystem         # flash the Pi-attached board + scrape UART
```

## Status: SIL-validated (board_sim)

`scripts/sim/sil_all.sh --only mem_subsystem` passes: board_sim boots the ELF headless
and scrapes the exact banner above. Because every layer is pure computation with
no register access, board_sim (Unicorn executing the real Cortex-M85 instruction
stream) reproduces the silicon result exactly -- the only modelled peripherals are
the CGC / MSTP / SysTick bring-up and the SCI8 console, the same path the
`epub_parse` sibling already validates. The host unit test
`tests/test_app_mem_subsystem.c` asserts the same counters and the
`vmem_crc=D1E67963` fingerprint, so the gate is SIM == HIL by construction.

There is no external hardware and no card -- it needs only a stock EVM UART -- so
the bench run is a formality once a board is free.
