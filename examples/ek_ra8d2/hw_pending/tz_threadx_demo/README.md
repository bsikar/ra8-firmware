# tz_threadx_demo -- TrustZone + ThreadX starter example

A minimal, teaching-focused example showing how the main e-reader application
(`apps/stand_alone/ereader`) works. 

This example splits CPU0 (Cortex-M85) into a Secure and a Non-Secure project:
1. **Secure Side (`tz_threadx_demo`)**: Handles secure boot, configures the SAU (Security Attribution Unit), hosts the Non-Secure Callable (NSC) veneers, and transitions to the Non-Secure world.
2. **Non-Secure Side (`tz_threadx_demo_ns`)**: Runs the ThreadX RTOS kernel and manages two user threads (UI and Worker) using NSC veneers for logging and peripheral initialization.

## How it works

1. **Secure Boot**: At reset, the primary M85 starts in Secure world. `SystemInit` sets up clocks and executes `ra8_trustzone_init` to define SAU regions and transition to Non-Secure reset handler (`ns_reset_handler`).
2. **BSS and VTOR**: `ns_reset_handler` zeroes the Non-Secure BSS memory and sets the NS Vector Table Offset Register (VTOR) so exceptions map to Non-Secure handlers.
3. **Substrate Init**: The NS handler calls `ra8_nsc_periph_init()` via the NSC veneer. Since peripheral registers (MSTP, CGC, etc.) are in Secure space, the veneer transitions to Secure world temporarily to set up hardware, then returns to Non-Secure.
4. **Multitasking**: The NS handler enters ThreadX via `tx_kernel_enter()`. `tx_application_define()` creates:
   - **UI Thread**: Periodically logs UI heartbeats.
   - **Worker Thread**: Periodically logs worker/sensor heartbeats.
5. **Veneer Logging**: Logs are emitted using `ra8_nsc_log_emit(...)` which copies messages to a secure scratch buffer and writes them to the ITM stimulus port.

## Running in the Emulator

```sh
make emu-tz_threadx_demo
```

This builds the secure and non-secure ELFs, then launches the `ra8_emulator` emulator. 
Expected terminal output will print heartbeats from both ThreadX threads:

```
[itm] [BOOT] INFO: tz_threadx_demo: Non-Secure world online!
[itm] [UI] UI thread started -- Simulating screen/event loop
[itm] [WORK] Worker thread started -- Processing background tasks
[itm] [UI] UI Loop: Refreshing screen layout...
[itm] [WORK] Worker Loop: Reading battery & sensor state via NSC veneers
...
```

## Files

| File | Role |
|------|------|
| [CMakeLists.txt](CMakeLists.txt) | Builds the two-project TrustZone structure and merges HEX outputs |
| [Makefile](Makefile) | Standalone build helper |
| [main.c](main.c) | Secure world fallback entry |
| [ns_main.c](ns_main.c) | Non-Secure entry point, ThreadX app initialization and threads |
| [linker_script.ld](linker_script.ld) | Memory map for the Secure executable |
| [ns_image.ld](ns_image.ld) | Memory map for the Non-Secure executable |
| [system_init.c](system_init.c) | Clocks and memory controller setup |
| [trustzone_init.c](trustzone_init.c) | SAU configuration and transition helper |
| [vector_table.c](vector_table.c) | Reset vectors for Secure boot |
