# cpu1_pingpong_ra8p1

The RA8P1's first dual-core example (RA8FW-496). The Cortex-M85 releases CPU1,
the Cortex-M33, and the two cores trade 10 ping-pong round trips through four
words of shared SRAM at 0x2210_0000. The M85 prints the verdict on the console:

```
cpu1_pingpong_ra8p1: 10 rounds PASS
```

It is the EK-RA8D2 `cpu1_pingpong` protocol (same magics, same poll budget)
written in Zig on both halves:

- `src/main.zig`: the M85 application (`CrossApp.zig_main`).
- `src/cpu1_main.zig`: CPU1's vector table, reset path and pong loop. There
  is no RTOS on CPU1, so no `uses` glue.
- `src/shared.zig`: the shared block.

CPU1 links with `libs/ra8_board_ra8p1/ld/linker_script_cpu1.ld` (MRAM_CPU1
0x020C_0000, 256K; SRAM_CPU1 0x2219_0000, 64K) and the M85 image keeps the
dual-core 768K MRAM length.

There is no CMakeLists.txt: `ra8_add_app()` cannot declare a Zig main, so this
app exists only in the Zig build graph. `zig build arm` emits
`cpu1_pingpong_ra8p1.elf` (with CPU1's image embedded) and
`cpu1_pingpong_ra8p1_cpu1.elf`. There is no RA8P1 board on the bench, so it is
checked in the emulator only.
