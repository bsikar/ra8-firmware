# camera_io_stream_demo

First app that links `ra8_camera_io`.

`ra8_camera_io` is one function, `ra8_camera_codec_encode_to_stream`, which
encodes a frame through an `ra8_camera` codec and writes the complete result to
an `ra8_io_stream` sink. It was split out of `ra8_camera` so capture-only
consumers do not inherit `ra8_io`, and that split is exactly why nothing ever
linked it: before this app it was compiled only by the host tests, whose fakes
stand in for both the codec and the sink. This app is that consumer.

## What it proves

1. A deterministic synthetic frame (SOI, a fixed byte ramp, EOI) is replayed
   through `ra8_camera_source_memory_init`, so the bytes that enter the source
   are known and every later comparison is a real assertion rather than a
   plausible-looking print.
2. The bridge writes the whole encoded frame into a RAM sink
   (`ra8_io_stream_ram_init`) with the zero-copy JPEG passthrough codec and an
   empty output buffer: the reported byte count, `ra8_io_stream_ram_used`, and
   a byte-for-byte compare of the sink against the source frame must all agree.
   That is the zero-copy path and the sink path checked together, which is what
   no host fake can do.
3. Two refusals are checked, because the bridge's own contract is mostly about
   refusing:
   - a NULL stream returns `k_ra8_err_null_ptr` and writes nothing;
   - a sink two bytes too small returns the sink's `k_ra8_err_no_mem` and
     reports the accepted prefix, instead of reporting a short write as
     success.

## Why the frame is synthetic

The demo moves a complete encoded byte stream; nothing in the path decodes it.
A recorded camera JPEG would make the app depend on a capture fixture without
testing one extra line of `ra8_camera_io`, so the frame is generated in place
and the payload is documented as a ramp, not as image data. The CEU source and
the real camera module stay out of scope: this is a consumer for the bridge, not
a camera bring-up.

## Run

Console is SCI8 over the J-Link OB VCOM at 115200.

```
just build camera_io_stream_demo
just emu camera_io_stream_demo
```

Expected banner:

```
camera_io_stream_demo: boot
camera_io_stream_demo: bridge PASS
```

A failing leg prints `camera_io_stream_demo: bridge FAIL err <code>`.

## Status

`hw_pending`. Nothing here touches a peripheral beyond the console, so
ra8_emulator runs it as is, but it has not been captured on the bench.
