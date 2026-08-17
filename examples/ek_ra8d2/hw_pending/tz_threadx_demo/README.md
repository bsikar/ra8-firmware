# tz_threadx_demo

A minimal, teaching-focused example of the structure the main e-reader
application (`apps/stand_alone/ereader`) uses: CPU0 (Cortex-M85) split into a Secure and a
Non-Secure project. The Secure side handles secure boot, configures the SAU,
hosts the Non-Secure Callable veneers, and transitions to the Non-Secure world;
the Non-Secure side runs the ThreadX kernel with two user threads that reach
back through NSC veneers for logging and peripheral init.

## The boot order

The sequence is the content here -- each step exists because the previous one
left the core somewhere it could not stay:

1. **Secure boot.** At reset the M85 starts in the Secure world. `SystemInit`
   sets up clocks, then `ra8_trustzone_init` defines the SAU regions and
   transitions to the Non-Secure reset handler.
2. **BSS and VTOR.** That handler zeroes the Non-Secure BSS and sets the NS
   Vector Table Offset Register, so exceptions map to Non-Secure handlers.
3. **Substrate init.** It then calls `ra8_nsc_periph_init()` through a veneer.
   Peripheral registers (MSTP, CGC and the rest) live in Secure space, so the
   veneer transitions back to Secure long enough to set the hardware up, then
   returns.
4. **Multitasking.** Finally it enters ThreadX, whose application define creates
   a UI thread and a worker thread.

Logging goes through `ra8_nsc_log_emit()`, which copies the message into a secure
scratch buffer before writing it to the ITM stimulus port -- the Non-Secure side
never touches the port itself.
