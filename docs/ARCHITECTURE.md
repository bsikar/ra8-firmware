# ra8-firmware architecture

How the firmware is put together, from reset vector to `main()`.

## Layers

```mermaid
graph TD
    APP["application main()<br/>an examples/ app or an apps/ product"]
    DRV["peripheral drivers<br/>libs/ra8_hal/src/ -- one per on-chip peripheral<br/>(timers, serial, analog, storage, graphics, DMA, ...)"]
    REG["register headers<br/>libs/ra8_hal/inc/ra8_*_regs.h -- hand-written from the HUM"]
    CORE["ra8_core<br/>err, check, log, time, pin validator, register guards,<br/>error handler, exception, infrastructure"]
    BOOT["boot<br/>libs/ra8_board_*/boot/ and ld/"]
    HW["Renesas R7KA8D2KFLCAC"]

    APP --> DRV
    DRV --> REG
    DRV --> CORE
    REG --> HW
    BOOT --> HW
```

`ra8_core` has no hardware dependencies and compiles on host and target with the
same flags. `ra8_hal` is the only layer that dereferences a peripheral address.
A driver is a register header plus `ra8_core`'s utilities -- error codes, the
pin validator, logging, IRQ-masked critical sections.

## How it runs

The layer diagram above is the dependency stack. This is the same firmware seen as a
running system -- two cores, two TrustZone worlds, a companion radio, and the four
boundary mechanisms that carry traffic between them.

<img src="diagrams/system_map.svg" alt="RA8 system map: cores, TrustZone worlds, the mailbox, the ESP32-C6 over SPI, and the three apps/ source categories" width="100%">

## Where code lives

```
examples/<tier>/.../<app>/   one standalone example per directory
apps/stand_alone/<product>/  a proving product, structured as its own repo
  main.c                     the application entry -- often the only file here
  CMakeLists.txt             a stub that calls ra8_add_app()
  Makefile                   a wrapper around cmake plus scripts/ helpers

  (overrides, present only when the app must diverge from the board defaults)
  vector_table.c  system_init.c  secure_exception.c  nmi_exception.c
  trustzone_init.c  linker_script.ld

libs/ra8_board_<board>/       the DEFAULT boot files and linker script
  boot/  ld/linker_script.ld

libs/                         the standard library -- see libs/README.md
```

Bare-metal has no `crt0.o` shipped by the toolchain, so the board layer *is*
`crt0.o`: it provides the vector table pinned to MRAM, the `.data` copy and
`.bss` zero, and a `SystemInit()` that has already set VTOR, the FPU enables and
NVIC priority grouping by the time `main()` runs. That is why an app is usually
one file. Dropping a same-named file into the app directory overrides the board
copy for that app alone -- divergent vector tables and linker scripts are
supported, they are just not the default.

### The build

The top-level `CMakeLists.txt` discovers every directory holding a `main.c` and
a `CMakeLists.txt`. All the logic is in one shared recipe,
`cmake/ra8_add_app.cmake`, so the per-app file only names the app:

```cmake
ra8_add_app(
    NAME blink
    STACK_BYTES 2200
    DESCRIPTION "Bare-metal blink firmware for RA8D2"
)
```

`ra8_add_app()` links the app's `main.c`, the boot files and the linker script
(the app-local copy if present, else the board's) and the `ra8_*` libraries,
`ra8_secure_app` among them. Its remaining options -- which board, which extra
libraries, whether the app skips the NSC layer -- are documented in that
file's header.
Adding an app is dropping the directory in; the next `make` finds it.

## Boot

```mermaid
graph TD
    RST["Reset_Handler<br/>board boot/vector_table.c"]
    SI["SystemInit -- board boot/system_init.c<br/>disable IRQ, VTOR to g_ra8_vector_table_start,<br/>CPACR CP10/CP11 (FPU), FPCCR LSPEN + ASPEN,<br/>ICIALLU + CCR.IC, CCR.DC, CCR.BP,<br/>NVIC priority grouping = 4 preempt bits"]
    CPY["copy .data from its MRAM load address to SRAM, zero .bss"]
    MAIN["main()<br/>ra8_infrastructure_init -- log backend, pin validator<br/>ra8_cgc_init -- PLL to CPUCLK0 at ~1 GHz<br/>ra8_time_init -- SysTick at 1 kHz<br/>application loop"]

    RST --> SI --> CPY --> MAIN
```

## Error handling

Every fallible function returns `ra8_err_t`: `k_ra8_ok` means the
post-conditions hold, and any other `k_ra8_err_*` is the caller's to handle.
Propagation goes through `ra8_check.h`:

| Macro | For |
|---|---|
| `RA8_CHECK_NULL_PTR(ptr, tag, msg)` | rejecting NULL at entry |
| `RA8_RETURN_ON_ERROR(err, tag, msg)` | propagating up the call stack |
| `RA8_ERROR_CHECK(err)` | halting on a fatal error -- init paths only |
| `RA8_ASSERT(cond, msg)` | programmer errors, never runtime conditions |

A fault that makes the system unsafe goes to `ra8_fatal_error()`, which masks
interrupts, logs, `BKPT #0`s to stop an attached debugger, and spins in `WFI`.
CPU exceptions go further: each of the four synchronous faults has a naked
trampoline that captures the stacked exception frame and exception number and
forwards to `ra8_exception_report()` for a full SCB dump.

## Dependency injection

`ra8_core` defines the vtables a driver injects through --
`ra8_pin_interface_t`, `ra8_time_interface_t` and `ra8_error_interface_t`,
with production instances
`g_ra8_gpio_pin_interface`, `g_ra8_time_interface_systick` and
`g_ra8_error_sink_log`. A driver that wants to be unit-testable takes them in
its `init()` config and calls through the vtable rather than reaching for a
global; tests plug in mocks that record every call.

## Clock tree after `ra8_cgc_init()`

The 24 MHz main crystal feeds PLL1 (integer multiply only -- the PLLCCR2
fractional path is unimplemented) to give CPUCLK0 at ~1 GHz for the Cortex-M85,
and from there:

| Divider | Clock | Rate |
|---|---|---|
| /4 | ICLK, PCKD, PCKE, MRICLK | 250 MHz |
| /8 | PCKA, PCKC, FCLK, BCLK | 125 MHz |
| /16 | PCKB | 62.5 MHz |

Drivers read the live value from `ra8_cgc_get_clock_hz()` rather than
hard-coding `k_ra8_pclkb_hz` -- the constants in `ra8_time_constants.h` are the
bring-up *targets*, not a promise about the running system.
