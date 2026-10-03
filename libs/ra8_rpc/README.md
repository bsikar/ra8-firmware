# ra8_rpc

One RPC core for the firmware and for the tools that talk to it: a frame
header, a codec that turns a plain Zig struct into bytes and back, and a
session of requests, responses and events on top. Freestanding Zig with no
heap and no libc; every buffer belongs to the caller.

Message sets are not in here. The firmware's app messages and the emulator's
session messages each live with their owner and share only this library, and
so do the transports that carry them. The one transport in this directory is
the in-memory loopback the tests run on.

## The frame

| Offset | Field    | Type  | Meaning                         |
|-------:|----------|-------|---------------------------------|
| 0      | `length` | `u32` | Payload bytes after the header  |
| 4      | `kind`   | `u16` | What the payload is; see below  |
| 6      | payload  |       | `length` bytes                  |

Everything is little-endian. `length` counts the payload only: the six header
bytes are never included, so an empty payload has a length of zero.

## The payload

A message is a struct, and its layout is derived from the struct at comptime.
Fields go out in declaration order with no padding:

- an 8, 16, 32 or 64-bit integer is its bytes, little-endian;
- an enum is its integer tag;
- a `[]const u8` is a `u32` length and then that many bytes;
- a tagged union is its integer tag and then the active field, which is any of
  the above, another tagged union, or `void` for nothing.

Nothing else is accepted; any other field type fails to compile. A union's tag
must have an explicit width (`union(enum(u8))` or a named enum), so the tag on
the wire never depends on how many fields the union has.

A slice has no size of its own, so its owner bounds each one by name, and that
bound is what gives every message a comptime maximum size:

```zig
const rpc = @import("ra8_rpc");

const Write = struct {
    addr: u32,
    data: []const u8,

    pub const max_len = .{ .data = 256 };
};

const Status = union(enum(u8)) {
    idle: void,
    busy: u16,
    failed: []const u8,

    pub const max_len = .{ .failed = 32 };
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
| `BadTag`    | An enum or union field holds a tag its type does not name         |
| `Trailing`  | Bytes are left over after the last field                          |

A failed encode leaves the output buffer untouched. `Oversize` is decided from
the claimed length alone, before the bytes are looked for, so a stream reader
can refuse a frame from its header without waiting for the body.

The encoding is canonical: a value has one encoding, and a payload that decodes
at all encodes back to the same bytes. The fuzz test leans on exactly that.

## The envelope

The frame kinds are pinned in `rpc.Kind`. Zero is never sent.

| Kind | Name       | Payload                                            |
|-----:|------------|----------------------------------------------------|
| 1    | `hello`    | `magic: u32`, `version: u16`, `caps: u32`          |
| 2    | `request`  | `id: u32`, `method: u16`, `args: []const u8`       |
| 3    | `response` | `id: u32`, `result`: tag `u8`, then `ok` or `err`  |
| 4    | `event`    | `topic: u16`, `payload: []const u8`                |
| 5    | `fault`    | `code: u16`, `version: u16`                        |

A response's `result` is a union: tag 0 is `ok` and carries the reply as bytes,
tag 1 is `err` and carries a `u16` code. `args`, `ok` and `payload` hold a
message already encoded by the codec above; the envelope carries it and does
not look inside. `rpc.Envelope(max_body)` makes the three traffic messages for
bodies of up to `max_body` bytes, and `max_frame` is the buffer that holds any
frame either side can send.

The refusal codes are pinned in `rpc.Code`. A handler reports its own failures
with values from `0x0100` up.

| Code | Name               | Meaning                                         |
|-----:|--------------------|-------------------------------------------------|
| 1    | `unknown_method`   | No handler answers that method                  |
| 2    | `bad_args`         | The arguments did not decode                    |
| 3    | `failed`           | The handler's reply broke its own bounds        |
| 4    | `version_mismatch` | The peer speaks another protocol version        |
| 5    | `bad_magic`        | The hello did not start with the magic          |
| 6    | `not_ready`        | Traffic arrived before the handshake finished   |

### Handshake

The client sends `hello` first: the magic `0x52384152` (the bytes `RA8R`),
protocol version 1, and a `u32` of capability bits whose meaning belongs to the
message set in use. A server that accepts it answers with its own `hello`, and
each side then holds the other's capabilities.

A side that cannot accept a hello sends a `fault` naming the reason and the
version it speaks, and returns `VersionMismatch` or `BadMagic` to its caller.
The side that receives the fault returns the same error to its own caller. So
a mismatch is an error at both ends and a frame on the wire, never a hang.
Requests and events before the handshake are `NotReady`.

## The session

Nothing blocks and nothing is called back. Each side is polled.

```zig
const Session = rpc.Envelope(256);
const Client = rpc.Client(4, 256); // four calls in flight

var rx: [Session.max_frame]u8 = undefined;
var tx: [Session.max_frame]u8 = undefined;
var client = Client.init(transport, &rx, my_caps);

try client.greet(&tx);
const id = try client.call(Write, Method.write, .{ .addr = 0, .data = chunk }, waiter, &tx);

while (try client.poll(&tx)) |incoming| switch (incoming) {
    .ready => |server_caps| {},
    .response => |r| {}, // r.id, r.waiter, r.result
    .event => |e| {}, // e.topic, e.payload
};
```

- **Pending calls.** A client holds a fixed table from request id to a `waiter`,
  a number of the caller's choosing that comes back with the response. A call
  with the table full is `TableFull` and sends nothing. Responses may arrive in
  any order. A response whose id is not waiting is `UnknownId`; it is never
  dropped quietly.
- **Dispatch.** A server is built over a comptime table of
  `.{ method, handler }` pairs. A handler is
  `fn (*Context, Args) rpc.Outcome(Reply)`; the server decodes `Args`, calls
  it, and encodes the `Reply`. An unknown method, arguments that do not decode
  and a handler's own refusal all go back to the caller as an `err` response,
  and the server carries on.
- **Events.** `server.emit(T, topic, value, &tx)` pushes one. They reach the
  client through `poll`, in the order sent, between whatever responses are
  also on the wire.

```zig
fn write(board: *Board, args: Write) rpc.Outcome(Done) { ... }

const Server = rpc.Server(Board, 256, .{
    .{ Method.write, write },
});
var server = Server.init(transport, &rx, &board, my_caps);
_ = try server.poll(&tx); // .idle, .greeted or .answered
```

## Transports

A transport is a context pointer and a vtable of three functions, injected the
way the rest of the firmware injects its dependencies:

| Function  | Contract                                                      |
|-----------|---------------------------------------------------------------|
| `send`    | Queue all of the bytes, or none of them and `LinkFull`        |
| `receive` | Copy waiting bytes into the buffer; zero if there are none    |
| `poll`    | Give the transport a turn; return how many bytes are waiting  |

A transport moves bytes and knows nothing about frames. It may deliver them in
any pieces; the session reassembles them in its receive buffer. A frame longer
than that buffer is `Oversize`.

`rpc.Loopback` joins two transports back to back over two caller-owned byte
rings. It is what the tests run on.

## Tests

`zig build test --summary all` runs nine roots:

- `codec_test`, `frame_test`, `envelope_test`: sizes, pinned numbers, and each
  refusal.
- `golden_test`: every message in `tests/messages.zig` against its checked-in
  frame under `tests/fixtures/`, in both directions, byte for byte. That
  includes a union message and one frame of each envelope kind.
- `loopback_test`, `link_test`, `pending_test`: the transport, frame
  reassembly, and the pending table.
- `session_test`: a client and a server over loopback through calls,
  out-of-order responses, interleaved events, a full table, an unknown id, an
  unknown method and a version mismatch in each direction.
- `fuzz_test`: damaged golden frames and random bytes through the decoder,
  then onto the wire of a live session towards each side. The seeded loops run
  every time; `zig build test --fuzz` drives the same properties with coverage
  guidance.

The fixtures are hex text, one field per line, written by hand from the rules
above and not captured from the encoder. They are plain files so that another
implementation of this format can be held to the same bytes.
