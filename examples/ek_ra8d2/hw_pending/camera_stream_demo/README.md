# camera_stream_demo

First app that links the optional `ra8_camera_io` bridge.

`ra8_camera_io` is kept separate from `ra8_camera` so capture-only consumers do
not inherit `ra8_io`. The consequence was that nothing in `apps/` or
`examples/` opted into both, so the bridge had host coverage
(`tests/misc/src/test_ra8_camera.c`) but no firmware consumer. This app is that
consumer.

## What it proves

1. A deterministic RGB888 test pattern stands in for a captured frame, built
   exactly as `ra8_camera_source_capture` would hand it over. No sensor, no CEU
   routing, no board straps.
2. `ra8_camera_codec_jpeg_sw` binds the software JPEG codec over a caller-owned
   RGB workspace and a caller-owned encoded-output buffer.
3. `ra8_io_stream_ram` is the sink, so the accepted bytes stay inspectable in
   SRAM rather than disappearing down a UART.
4. `ra8_camera_codec_encode_to_stream` performs the single bounded operation the
   bridge exists for: encode, then write the complete encoded frame. The verdict
   compares the reported written count against the bytes the sink actually
   captured, which is the property the bridge promises and the reason it returns
   `k_ra8_err_invalid_size` on a short write.

The bridge deliberately does not flush, so a caller can append a protocol
trailer or batch frames. This app relies on that: the RAM sink is read back
directly instead of being committed.

## Run

Console is SCI8 over the J-Link OB VCOM at 115200.

```
just build camera_stream_demo
just emu camera_stream_demo
```

Expected banner:

```
camera_stream_demo: boot
camera_stream_demo: encode-to-stream bytes <n> PASS
```

## Status

`hw_pending`. Nothing here touches a peripheral beyond the console, so
ra8_emulator runs it as-is, but it has not been captured on the bench yet.
