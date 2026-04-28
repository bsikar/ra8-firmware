# ra8d2-firmware architecture

Quick walkthrough of how the firmware is organised. Living
document -- expand as new subsystems land.

## Layers

```
    +----------------------------------------------------+
    |                application main()                  |   examples/<name>/main.c
    +----------------------------------------------------+
    |                    drivers                          |   libs/ra_hal/src/
    |        gpio | uart | iic | spi | adc | gpt | agt    |   libs/ra_hal/inc/
    |       cgc | iwdt | wdt | crc | rtc | cac | elc      |
    |       icu | dmac | dtc | canfd | xspi | usb | glcdc |
    |       sdramc                                         |
    +----------------------------------------------------+
    |              register headers (hand-written)        |   libs/ra_hal/inc/ra8d2_*_regs.h
    +----------------------------------------------------+
    |                    ra_core                          |   libs/ra_core/inc/
    |  err | check | log | bit_constants | time_constants |   libs/ra_core/src/
    |  port_constants | gpio_constants | pin_validator    |
    |  error_handler | exception | time | register_guard  |
    |  register_protection | infrastructure               |
    +----------------------------------------------------+
    |                    boot / SystemInit                |   src/boot/
    |  vector_table.c | system_init.c | linker_script.ld  |
    +----------------------------------------------------+
    |                     hardware                        |   Renesas R7KA8D2KFLCAC
    +----------------------------------------------------+
```

`ra_core` has no hardware dependencies; it compiles on host and
target with the same flags. `ra_hal` is the only layer that
dereferences peripheral addresses. Drivers build on top of a
register header plus the utilities in `ra_core` (error codes,
pin validator, logging, IRQ-masked critical sections).

## Source-tree layout: `src/` vs `examples/` vs `libs/`

Three distinct roles, often confused:

```
src/                 ← the C runtime + boot code (everyone shares it)
  boot/
    vector_table.c     112-IRQ Cortex-M85 vector table + Reset_Handler
    system_init.c      VTOR, FPU enable, priority grouping
    secure_exception.c Secure-side fault entry
    trustzone_init.c   SAU bring-up (no-op when RA_TRUSTZONE_ENABLE is OFF)
  inc/                 Internal headers shared between src/* TUs
  secure_app/          Ring 5 secure-side code (key vault, secure veneer table)
  linker_script.ld     Memory map, .text/.data/.bss/.vectors placement, OFS sections

libs/                ← the standard library (everyone links it)
  ra_core/             err, log, time, pin validator, register guards (no HW deps)
  ra_hal/              every peripheral driver + register header
  ra_nsc/              TrustZone Non-Secure-Callable veneers
  ra_net_pal/          Ethernet PAL bridging the HAL to lwIP / similar
  ra_usb_pal/          USB PAL bridging the HAL to CherryUSB / similar

examples/            ← the application (one-file main.c per example)
  blink/main.c         Raw register-poke blink (~250 lines)
  blink_hal/main.c     Same blink, built on the HAL (~150 lines)
  <future>/main.c      Add a new example by dropping a directory here
```

### Why every example is tiny

Each `examples/<name>/main.c` only contains the application's `main()`
function. It assumes:

- The vector table exists and is pinned to MRAM (provided by
  `src/boot/vector_table.c`)
- `Reset_Handler` ran the `.data` copy + `.bss` zero before `main()`
- `SystemInit()` set VTOR, the FPU coprocessor enables, and NVIC
  priority grouping (provided by `src/boot/system_init.c`)
- The HAL libraries in `libs/` are linked and ready to use

So `blink_hal/main.c` only has to set up its specific peripherals
(GPIO, SysTick) and run its loop -- everything underneath is
provided by `src/` and `libs/`.

### What the CMake glob actually does

Every firmware build is the same shape:

```cmake
file(GLOB APP_SOURCES_ROOT
    ${CMAKE_SOURCE_DIR}/src/*.c        # any plain src/*.c (currently none)
    ${EXAMPLE_DIR}/*.c                 # the chosen example's main.c
)
file(GLOB_RECURSE APP_BOOT_SOURCES   ${CMAKE_SOURCE_DIR}/src/boot/*.c)
file(GLOB_RECURSE APP_SECURE_SOURCES ${CMAKE_SOURCE_DIR}/src/secure_app/*.c)
# ... plus libs/ra_core/, libs/ra_hal/, libs/ra_nsc/, libs/ra_*_pal/
```

`-DEXAMPLE=<name>` (defaulting to `blink`) picks which example's
`main.c` is compiled. Everything else is the same across builds.

### Mental model: hosted-OS analogy

```
hosted Linux/macOS C program        ra8d2-firmware
─────────────────────────────       ─────────────────────────
crt0.o (runtime)                    src/boot/
libc / libm                         libs/
your code (main.c, ...)             examples/<name>/main.c
```

On a hosted system you don't *see* `crt0.o` because the toolchain
ships it. On bare-metal embedded, you have to write it yourself --
and `src/boot/` is exactly that.

Adding a new example never requires editing anything in `src/` or
`libs/`; just create `examples/foo/main.c` and run
`make example-foo`. Touching `src/` is a project-wide change to how
the chip boots, not a per-example concern.

## Boot sequence

```
    power / reset
         |
         v
    +--------------+
    | Reset_Handler|   src/boot/vector_table.c
    +-----+--------+
          | calls
          v
    +--------------+
    |  SystemInit  |   src/boot/system_init.c
    |              |
    |  1. disable IRQ
    |  2. VTOR <- g_ra_vector_table_start
    |  3. CPACR CP10/CP11 full access (FPU)
    |  4. FPCCR LSPEN + ASPEN (lazy stacking)
    |  5. ICIALLU + CCR.IC  (I-cache on)
    |  6. CCR.DC            (D-cache on)
    |  7. CCR.BP            (branch predictor on)
    |  8. NVIC priority grouping = 4 bits preempt
    +-----+--------+
          | returns
          v
    +--------------+
    | Reset_Handler|
    |              |
    |  copy .data from MRAM LOAD addr to SRAM
    |  zero .bss
    +-----+--------+
          | jumps
          v
    +--------------+
    |    main()    |   src/main.c
    |              |
    |  ra_infrastructure_init()  <- log backend, pin validator
    |  ra_cgc_init()              <- PLL to CPUCLK0 @ ~1 GHz
    |  ra_time_init(cpuclk)       <- SysTick 1 kHz
    |  application loop
    +--------------+
```

## Error handling

Every function that can fail returns `ra_err_t`. The only two
exits from normal control flow are:

1. `k_ra_ok` -- function completed successfully, post-conditions hold.
2. Any other `k_ra_err_*` value -- caller must handle.

Propagation is done through the macros in `ra_check.h`:

- `RA_CHECK_NULL_PTR(ptr, tag, msg)` -- reject NULL at entry.
- `RA_RETURN_ON_ERROR(err, tag, msg)` -- propagate up the call stack.
- `RA_ERROR_CHECK(err)` -- halt on fatal error (init paths only).
- `RA_ASSERT(cond, msg)` -- programmer errors, never runtime conditions.

Faults that make the system unsafe go to
`internal_ra_fatal_error()` which masks interrupts, logs the
failure, `BKPT #0`s to stop an attached debugger, and spins in
`WFI`. CPU exceptions go one step further: each of the four
synchronous faults has a naked trampoline that grabs the stacked
exception frame + exception number and forwards to
`ra_exception_report()` for a full SCB diagnostic dump.

## Dependency injection

Three abstract vtables are defined in `ra_core`:

- `ra_pin_interface_t`   -- any pin driver (production: `g_ra_gpio_pin_interface`)
- `ra_time_interface_t`  -- any time source (production: `g_ra_time_interface_systick`)
- `ra_error_interface_t` -- any error sink  (production: `g_ra_error_sink_log`)

Drivers that want to be unit-testable take these interfaces in
their `init()` config struct and forward calls through the vtable
instead of reaching for global functions. Tests plug in mock
vtables that record every call.

## Clock tree target (after `ra_cgc_init()`)

```
    24 MHz main XTAL
         |
         v
    +------------+
    |   PLL1     |
    |  x ~41.66  |   (integer mul today; PLLCCR2 fractional TODO)
    +------+-----+
           |
           v ~1000 MHz
    +-----------+
    |  CPUCLK0  |  <--- Cortex-M85 main core
    +-----------+
           |
           +--- /4 -> ICLK    = 250 MHz
           +--- /8 -> PCKA    = 125 MHz
           +-- /16 -> PCKB    = 62.5 MHz
           +--- /8 -> PCKC    = 125 MHz
           +--- /8 -> PCKD    = 125 MHz
           +--- /4 -> PCKE    = 250 MHz
           +-- /16 -> FCLK    = 62.5 MHz
           +--- /8 -> BCLK    = 125 MHz
           +-- /16 -> MRICLK  = 62.5 MHz
```

Drivers query the live values via `ra_cgc_get_clock_hz()` rather
than hard-coding `k_ra_pclkb_hz` -- the constants in
`ra_time_constants.h` are only the bring-up *targets*.
