# audio_memory_source_demo

First app that links `ra8_audio`'s in-memory replay backend.

`ra8_audio` already had a firmware consumer, but only ever through the PDM
backend: `pdm_mic_demo` and the C6 camera server's audio half both call
`ra8_audio_source_pdm_init` and then stream or capture. Nothing bound the
second backend, so the seam the facade exists for -- an app swapping its
capture source without touching its own logic -- was never exercised outside
the host tests, and `ra8_audio_source_get_info` (the call that lets an app size
its storage from the source instead of hard-coding the geometry) had no caller
at all. This app is that consumer. See #1349.

## What it proves

1. A caller-owned interleaved s16le frame is validated, bound through
   `ra8_audio_source_memory_init`, and the handle comes back pointing at the
   app's own state.
2. `ra8_audio_source_get_info` reports the source's fixed contract, and the app
   sizes its capture buffer from that answer rather than from the constant it
   used to build the frame.
3. A capture lands in the app's buffer byte for byte, and the returned view
   borrows that buffer (`frame.data == buffer.data`) instead of handing back a
   pointer into the source.
4. Replay is repeatable: the buffer is wiped and a second capture reproduces
   the same bytes, which is what makes the memory backend usable as a fixture.
5. The undersized-buffer refusal is real, not advisory: a buffer one byte short
   is rejected with `k_ra8_err_invalid_size` and the buffer is left untouched.
6. The memory backend publishes no streaming operation, so
   `ra8_audio_source_stream_start` refuses with `k_ra8_err_not_supported` and
   the callback never fires. The facade's null-operation branch is unreached by
   any other app, because PDM implements every operation.
7. `ra8_audio_source_stop` unbinds the handle, and a `get_info` afterwards
   fails `k_ra8_err_not_initialized` rather than reading freed state.

Each leg prints its own failure line, so a run says which one broke rather than
just failing.

## Why it needs no hardware

The source is a PCM frame in `.rodata` and the sink is a buffer in `.bss`;
nothing touches PDM, DMA or a codec. Only the SCI8 / J-Link OB VCOM console is
used, to report the verdict. That makes the app deterministic, so
`ra8_emulator` runs it as is.

It sits under `hw_pending` because it has not been captured on the bench yet,
not because it needs a peripheral.

## Expected output

```
audio_memory_source_demo: boot
audio_memory_source_demo: memory source PASS
```

Any leg that fails prints `audio_memory_source_demo: leg <n> <what> FAIL`
before the verdict line.
