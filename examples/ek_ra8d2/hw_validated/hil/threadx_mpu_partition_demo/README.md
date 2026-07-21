# threadx_mpu_partition_demo

Eclipse ThreadX + Arm v8-M MPU partition HIL test for the EK-RA8D2.
Programs a small static MPU region table at boot via `ra8_mpu_configure`
and then runs a single ThreadX worker that blinks LED1 at 1 Hz to
prove the MPU configuration did not wedge ordinary code execution.

## Region layout

| # | Region    | Base         | Size     | Privileged | Unprivileged | Exec |
|---|-----------|--------------|----------|------------|--------------|------|
| 0 | MRAM      | `0x02000000` | 1 MiB    | RO         | RO           | yes  |
| 1 | SRAM      | `0x22000000` | 1 MiB    | RW         | RW           | no   |
| 2 | Periph.   | `0x40000000` | 256 MiB  | RW         | none         | no   |

`PRIVDEFENA` is left enabled so privileged accesses outside the
declared regions fall through to the architectural defaults; this
keeps the kernel's bookkeeping pages reachable without having to
enumerate them. MAIR slots default to `0` (device-nGnRnE) -- adequate
for a HIL test. A real partitioning policy would split secure /
non-secure worlds and add per-thread sub-regions.

## What it does

1. `ra8_mpu_configure(&s_mpu_cfg)` -- install the 3-region table.
2. `ra8_board_led_init(k_ra8_board_led1)` -- bring LED1 up as output.
3. `tx_kernel_enter()` -- spin the ThreadX scheduler.
4. Worker thread (`mpu_blink`, prio 4, 1 KiB stack) toggles LED1
   every 1000 ticks (1 Hz).

`SysTick_Handler` is overridden to forward into `_tx_timer_interrupt`
for the 1 kHz kernel tick.

## Build + flash

From the repo root:

```sh
make threadx_mpu_partition_demo
bash scripts/dev/flash.sh build/threadx_mpu_partition_demo/threadx_mpu_partition_demo.hex
```

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1 init/toggle (per EK-RA8D2 v1 UM
Table 24 "EK-RA8D2 Board LED Functions" p 31).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual
(R20UT5523EG0101 Rev 1.01) Table 24 p 31, HUM (R01UH1065EJ0130) Ch
"Memory Protection Unit (MPU)" + Ch "SysTick", and the ARMv8-M
Architecture Reference Manual MPU programming model.
