# ereader_image

Decodes a baked RGB PNG cover through `ra8_img_decode_blit()`,
nearest-neighbour scales it to fit a fixed RGB565 framebuffer in internal SRAM,
and hashes the framebuffer (#106) -- the same decode, scale and blit path the
e-reader uses for cover art and in-chapter `<img>` figures. Allocation comes
only from a fixed SRAM bump arena, so the decode reaches no `malloc` (NASA P10
Rule 3). Headless -- no panel, SDRAM, touch or SD.

A fixed fixture, integer scale math and a fixed RGB565 pack make it
deterministic, so the same hash appears on host, under the emulator and on
silicon, and drift in the stb_image decoder, the scale math, the pack or the
toolchain output trips it.

One non-obvious constraint this gate caught: stb_image otherwise compiles its
global failure-reason and flag state as `_Thread_local`, and emulated TLS has no
runtime on this bare-metal target, so the decode HardFaults mid-way. The
single-TU build defines `STBI_NO_THREAD_LOCALS` to force plain statics.
