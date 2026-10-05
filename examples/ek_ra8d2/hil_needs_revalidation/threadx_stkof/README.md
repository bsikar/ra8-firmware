# threadx_stkof

ThreadX on CPU0 (Cortex-M85) with one thread that overflows its stack on
purpose. It's the firmware image behind the emulator's ThreadX stack-limit
check (RA8EMU-243), and the first example with a Zig entry instead of a
`main.c` (RA8FW-503).

## What it does

1. `main` brings up the clocks and the SCI8 console, prints `stkof: start`
   and enters ThreadX.
2. `tx_application_define` registers a stack-error callback and creates one
   thread with a 1 KiB stack.
3. The thread recurses well past that stack. The port has PSPLIM set to the
   stack start, so the first push below it raises a UsageFault with
   UFSR.STKOF.
4. The image's `UsageFault_Handler` follows the ThreadX port's STKOF path
   into `_tx_thread_stack_error_handler`, which calls the callback. Any other
   UsageFault still goes to the board's fault reporter.

## Expected console

```
stkof: start
stkof: thread=overflow caught
stkof: PASS
```

If the recursion returns without a fault, the thread prints `stkof: FAIL`
instead. The image parks after its verdict either way.

## Build

`zig build arm` builds it with the rest of the cross table. There's no
`CMakeLists.txt`: an app with a Zig entry exists only in the Zig graph.
