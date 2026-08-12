# cache_hal_enable_demo

Single-core (Cortex-M85) demo that boots with the **L1 I-cache + D-cache brought
up through the `ra8_cache` HAL** and proves the core still runs correctly with
them on. It is the HAL-driven twin of `cache_mpu_hil`: the same boot posture and
the same cacheable round-trip, but the caches are enabled by
`ra8_cache_icache_enable()` / `ra8_cache_dcache_enable()` instead of the shared
boot's hand-rolled `internal_enable_icache` / `internal_enable_dcache` register
pokes (issue #577).

## Status: hw_pending (passes ra8_emulator; awaiting on-silicon HIL run)

`ra8_emulator` models memory byte-exact and does **not** model the L1 D-cache, so
the cacheable-RW step passes trivially off-target -- the cache hazard this app
guards against is only real on the chip. The emulator run proves the app boots
and reports PASS with the HAL-driven cache boot path compiled in; the bench run
proves the same on real silicon.

## What it does

The app's build defines both `RA8_BOOT_ENABLE_CACHE_MPU` and
`RA8_BOOT_CACHE_VIA_HAL` (see `CMakeLists.txt`). The shared boot
`SystemInit()` (`libs/ra8_board_ek_ra8d2/boot/system_init.c`) therefore:

1. programmes the 5-region MPU (`internal_mpu_init()`), then
2. enables the caches through the HAL:
   ```c
   ra8_cache_icache_enable();   /* ICIALLU, then CCR.IC  */
   ra8_cache_dcache_enable();   /* set/way invalidate, then CCR.DC */
   ```
   -- exactly the ICIALLU + CCR.IC / CCR.DC sequence the raw
   `internal_enable_icache` / `internal_enable_dcache` helpers perform, now lifted
   into `libs/ra8_hal` `ra8_cache`. Each `*_enable` runs its architectural
   invalidate before setting the CCR bit, so it is safe from a cold cache at boot.
3. enables the branch predictor.

The raw-poke helpers stay compiled in the shared boot as the untouched reference
path; only an app that opts in with `RA8_BOOT_CACHE_VIA_HAL` takes the HAL route.

Every byte `main()` touches then runs with the L1 caches + MPU live:

| Step | MPU region | Memory type | Proves |
|------|------------|-------------|--------|
| 1. Cacheable RW | 1 (`0x22000000`, M85-private SRAM, 1 MiB) | Normal WB/WA, cacheable | A 4 KiB `.bss` buffer fills with `buf[i] = (uint8_t)(i*31+7)`, is clean+invalidated back to SRAM via `ra8_cache_dcache_clean_invalidate_by_addr()`, then reads back byte-for-byte -- the write-back + refill path works with the HAL-enabled D-cache. |

On pass the app prints, once, over the VCOM console:

```
cache_hal_enable_demo: L1-cache-via-HAL PASS
```

and mirrors the verdict over `ra8_log` (the emulator echoes it as an `[itm]`
line). On any mismatch it prints a distinct `... FAIL` line (never containing
`PASS`) and parks. The core ends every path in WFI.

## Build / run

```sh
make                # cross-compile build/cache_hal_enable_demo.elf
make flash          # J-Link load onto an EK-RA8D2
```

Console: SCI8 on PD02/PD03 over the on-board J-Link OB VCOM bridge, 115200 8N1.

## See also

- `libs/ra8_hal/inc/ra8_cache.h` -- the cache maintenance + enable/disable HAL.
- `examples/ek_ra8d2/hw_validated/hil/cache_mpu_hil` -- the raw-poke reference twin.
