# ra8_rpc

The wire format under the firmware's RPC: a six-byte frame header, and a codec
that turns a plain Zig struct into bytes and back. Freestanding Zig with no
heap and no libc; every buffer belongs to the caller.

This library stops at bytes. Request, response and event kinds, id
correlation, dispatch and transports are built on top of it, not in it. The
`kind` in the header is an opaque `u16` here.

## The frame

| Offset | Field    | Type  | Meaning                                   |
|-------:|----------|-------|-------------------------------------------|
| 0      | `length` | `u32` | Payload bytes after the header            |
| 4      | `kind`   | `u16` | What the payload is; opaque at this layer |
| 6      | payload  |       | `length` bytes                            |

Everything is little-endian. `length` counts the payload only.

## The payload

A message is a struct, and its layout is derived from the struct at comptime.
Fields go out in declaration order with no padding:

- an 8, 16, 32 or 64-bit integer is its bytes, little-endian;
- an enum is its integer tag;
- a `[]const u8` is a `u32` length and then that many bytes.

Nothing else is accepted; any other field type fails to compile. A slice has
no size of its own, so the message bounds each one by name, and that bound is
what gives every message a comptime maximum size:

```zig
const rpc = @import("ra8_rpc");

const Write = struct {
    addr: u32,
    data: []const u8,

    pub const max_len = .{ .data = 256 };
};

var buf: [rpc.frame.maxSize(Write)]u8 = undefined;
const bytes = try rpc.frame.encode(Write, kind, .{ .addr = 0x22000000, .data = chunk }, &buf);

const got = try rpc.frame.split(bytes, rpc.codec.maxSize(Write));
const write = try rpc.codec.decode(Write, got.payload);
```

Decoded slices point into the input buffer. Nothing is copied and nothing is
allocated, so a decoded message lives exactly as long as the bytes it came from.

## What is refused

Nothing is truncated to fit and nothing is read past the end of its input.

| Error       | When                                                              |
|-------------|-------------------------------------------------------------------|
| `NoSpace`   | The output buffer is smaller than the encoding                    |
| `Oversize`  | A slice is past its bound, or a frame is past the caller's limit  |
| `Truncated` | The input ended before the header, the payload or a field did     |
| `BadTag`    | An enum field holds a value its type does not name                |
| `Trailing`  | Bytes are left over after the last field                          |

A failed encode leaves the output buffer untouched. `Oversize` is decided from
the claimed length alone, before the bytes are looked for, so a stream reader
can refuse a frame from its header without waiting for the body.

The encoding is canonical: a value has one encoding, and a payload that decodes
at all encodes back to the same bytes. The fuzz test leans on exactly that.

## Tests

`zig build test --summary all` runs four roots:

- `tests/codec_test.zig` and `tests/frame_test.zig`: sizes, and each refusal.
- `tests/golden_test.zig`: every message in `tests/messages.zig` against its
  checked-in frame under `tests/fixtures/`, in both directions, byte for byte.
- `tests/fuzz_test.zig`: damaged golden frames and random bytes through the
  decoder. The seeded loops run every time; `zig build test --fuzz` drives the
  same property with coverage guidance.

The fixtures are hex text, one field per line, written by hand from the rules
above and not captured from the encoder. They are plain files so that another
implementation of this format can be held to the same bytes.
