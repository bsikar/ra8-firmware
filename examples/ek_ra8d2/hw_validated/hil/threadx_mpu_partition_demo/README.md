# threadx_mpu_partition_demo

Installs a small static Armv8-M MPU region table at boot via
`ra8_mpu_configure`, then runs one ThreadX worker blinking LED1 to prove the
partition did not wedge ordinary code execution. It is the positive-path
counterpart to `mpu_partition_simple`, which deliberately provokes a fault.

| # | Region     | Base         | Size    | Privileged | Unprivileged | Exec |
|---|------------|--------------|---------|------------|--------------|------|
| 0 | MRAM       | `0x02000000` | 1 MiB   | RO         | RO           | yes  |
| 1 | SRAM       | `0x22000000` | 1 MiB   | RW         | RW           | no   |
| 2 | Peripheral | `0x40000000` | 256 MiB | RW         | none         | no   |

`PRIVDEFENA` is left enabled, so privileged accesses outside the declared
regions fall through to the architectural defaults. That is what keeps the
kernel's own bookkeeping pages reachable without enumerating them, and clearing
it is how a table like this wedges a scheduler. MAIR slots stay at 0
(device-nGnRnE), adequate here but not a partitioning policy: a real one would
split secure from non-secure and add per-thread sub-regions.

There is no separate failure counter, because a broken partition and a wedged
scheduler surface identically -- the worker's counter stops advancing either
way.

`SysTick_Handler` forwards into `_tx_timer_interrupt` for the kernel tick.
LEDs per EK-RA8D2 v1 UM Table 24 p 31; MPU programming model per HUM
R01UH1065EJ0130 Ch "Memory Protection Unit (MPU)" and the Armv8-M
Architecture Reference Manual.
