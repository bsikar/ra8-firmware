# cache_mpu_hil

Single-core (Cortex-M85) HIL validator that boots with the **L1 I-cache +
D-cache and the 5-region MPU enabled** (`RA8_BOOT_ENABLE_CACHE_MPU`) and proves
the core still runs correctly with them on. This is the on-silicon arm of the
T4 cache-coherency chain: the shared boot's cache + MPU path is exercised
end-to-end and the result is scraped over the J-Link OB VCOM console.

## Status: hw_pending (passes board_sim; awaiting on-silicon HIL run)

`board_sim` models memory byte-exact and does **not** model the L1 D-cache, so
the cacheable-RW step passes trivially in simulation -- the cache hazard this
app guards against is only real on the chip. The sim run proves the app boots
and reports PASS with the cache + MPU boot path compiled in; the bench run
proves the same on real silicon (where a missing clean/invalidate or a
mis-mapped MPU region would corrupt data or fault).

## What it does

`SystemInit()` (shared boot, `libs/ra8_board_ek_ra8d2/boot/system_init.c`) runs
with `RA8_BOOT_ENABLE_CACHE_MPU` defined for this app, so it programmes the MPU
and enables the I-cache, D-cache, and branch predictor **before** `main()`.
Every byte the self-test touches therefore runs with the caches + MPU live.

| Step | MPU region | Memory type | Proves |
|------|------------|-------------|--------|
| 1. Cacheable RW | 1 (`0x22000000`, M85-private SRAM, 1 MiB) | Normal WB/WA, cacheable | A 4 KiB `.bss` buffer fills with `buf[i] = (uint8_t)(i*31+7)`, is clean+invalidated back to SRAM, then reads back byte-for-byte -- the write-back + refill path works. |
| 2. RO MRAM const + code | 0 (`0x02000000`, MRAM, 1 MiB) | RO + execute, cacheable | A non-inlined helper in MRAM `.text` sums a `const` table in MRAM `.rodata` via volatile loads -- code executes through the I-cache and the RO MRAM region is readable. |
| 3. Device MMIO | 3 (`0x40000000`, peripherals, 128 MiB) | Device-nGnRE (uncached) | The live SYSTEM `SCKDIVCR` register reads back the value `ra8_cgc_init()` programmed (`0x32233432`) through `ra8_sys_sckdivcr()` -- peripheral MMIO is mapped Device (not cached) and accessible. |

On all-pass the app prints, once, over the VCOM console:

```
cache_mpu_hil: cache+mpu PASS
```

and mirrors the verdict over `ra8_log` (the emulator echoes it as an `[itm]`
line). On any mismatch it prints a distinct `... FAIL` line (never containing
`PASS`) and parks. The core ends every path in WFI.

## Why region 4 is not exercised here

MPU region 4 (the shared M85<->M33 SRAM bank at `0x22100000`, mapped Normal
**non-cacheable**) keeps cross-core hand-offs coherent with the M85 D-cache on
and needs no software maintenance. That coherency is a dual-core property and
is validated by the dual-core mailbox / ping-pong apps; `cache_mpu_hil` is
deliberately single-core and only proves the M85 itself runs correctly with the
caches + MPU enabled.

## Build + run

From the repo root:

```sh
make cache_mpu_hil                 # cross-compile -> build/cache_mpu_hil.elf / .hex / .bin
make flash-cache_mpu_hil           # flash via on-board J-Link OB
make sim-cache_mpu_hil             # boot the real .elf on the board_sim CPU emulator
```

Headless `board_sim` (one-shot banner, idle-stop):

```sh
BOARD_SIM_WALL_S=15 BOARD_SIM_IDLE_STOP=1 \
  tools/ra8_emulator/build/ra8_emulator \
    examples/ek_ra8d2/hw_validated/hil/cache_mpu_hil/build/cache_mpu_hil.elf \
    --panel tools/ra8_emulator/panels/ek_ra8d2.toml
```

The VCOM console line `cache_mpu_hil: cache+mpu PASS` (or an `[itm]` mirror of
it) appears in the output.

## HIL gate (`hil.conf`)

`uart_scrape` mode: the Pi rig reads `/dev/ttyACM0` at 115200 8N1 and gates on
the success banner, failing on any `FAIL|HardFault|TIMEOUT|panic`.

```
HIL_MODE=uart_scrape
HIL_EXPECT="cache_mpu_hil: cache+mpu PASS"
HIL_EXPECT_NEGATIVE="FAIL|HardFault|TIMEOUT|panic"
HIL_TIMEOUT_S=15
```
