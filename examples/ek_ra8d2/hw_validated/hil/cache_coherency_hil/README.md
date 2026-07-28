# cache_coherency_hil

Dual-core HIL validator: prove the Cortex-M85 (CPU0) and Cortex-M33 (CPU1) stay
coherent across shared on-chip SRAM **with the M85 data cache ENABLED**, by
routing every cross-core hand-off through the boot's non-cacheable MPU region.

## What it proves

This M85 image is built with `RA8_BOOT_ENABLE_CACHE_MPU` (set in
`CMakeLists.txt`), so the shared boot
(`libs/ra8_board_ek_ra8d2/boot/system_init.c`) enables the MPU + I-cache +
D-cache before `main()` runs. The shared message struct
(`cache_coherency_shared.h`) is pinned at `0x22100000` (SRAM2), which the boot's
**MPU region 4** maps as Normal **non-cacheable**. Because of that, the
M85<->M33 hand-off needs **no** software cache maintenance
(`ra8_cache_dcache_clean_by_addr` / `..._invalidate_by_addr`): a cacheable
placement would let the M85 read a stale `pong_payload` from its own D-cache (or
hide its `ping_payload` write from the cacheless M33), and the round-trip would
mismatch. The non-cacheable region removes exactly that hazard.

The Cortex-M33 has no data cache, so coherency is one-sided -- only the M85 cache
matters. ra8_emulator models byte-exact memory (no D-cache), so the test passes
there trivially; the cache hazard is only real on silicon.

## How it runs

1. The M85 zeroes the shared block and releases the M33 with `ra8_cpu1_release`
   (HUM Ch 2.9.1).
2. Each round `r`: M85 writes `ping_payload = 0x1234 + r`, `DSB`, bumps
   `ping_seq`; the M33 echoes `pong_payload = ping_payload + 0x30ED`
   (`= 0x4321 + r`) and bumps `pong_seq`; the M85 verifies the echo.
3. `g_cache_coherency_match` advances once per verified round;
   `g_cache_coherency_mismatch` advances on a bounded-wait timeout or a wrong
   echo. After the first 8 verified rounds the success banner is emitted once
   over the SCI8 / J-Link OB VCOM console and over ITM:

   ```
   cache_coherency_hil: 8 rounds PASS
   ```

The loop then keeps round-tripping forever so the J-Link memprobe gate
(`hil.conf`) sees `g_cache_coherency_match` advance across its sample window. A
one-shot counter frozen after 8 rounds (e.g. a terminal `WFI`) would read an
unchanged value at both probe halts and fail the delta check -- this mirrors the
proven `cpu1_pingpong` structure. In ra8_emulator the run ends on the
`RA8_EMU_WALL_S` cap after the banner has already printed.

## Build + run

```sh
make cache_coherency_hil        # ARM cross-build (M85 + embedded M33 image)

# headless ra8_emulator (no GUI):
RA8_EMU_WALL_S=15 RA8_EMU_IDLE_STOP=1 \
  tools/ra8_emulator/build/ra8_emulator \
  examples/ek_ra8d2/hw_validated/hil/cache_coherency_hil/build/cache_coherency_hil.elf \
  --panel tools/ra8_emulator/panels/ek_ra8d2.toml
```

## HIL gate (`hil.conf`)

`HIL_MODE=jlink_memprobe` -- `g_cache_coherency_match` must advance >= 8 over a
5 s window and `g_cache_coherency_mismatch` must stay at 0. memprobe is the
robust gate for a dual-core counter app (no UART round-trip in the hot loop).
Stock EK-RA8D2, no add-on hardware.
