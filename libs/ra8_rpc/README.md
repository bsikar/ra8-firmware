# ra8_rpc

One RPC core for the firmware and for the tools that talk to it: a frame
header, a codec that turns a plain Zig struct into bytes and back, and a
session of requests, responses and events on top. Freestanding Zig with no
heap and no libc; every buffer belongs to the caller.

Message sets are not in here. The firmware's app messages and the emulator's
session messages each live with their owner and share only this library, and
so do the transports that touch an operating system or a wire. This directory
holds three that touch neither: the in-memory loopback the tests run on, a
transport over any queue of fixed-size messages, and a transport over a pair
of rings in memory two cores share.

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
| `poll`    | Give the transport a turn; zero if no bytes are waiting       |

A transport moves bytes and knows nothing about frames. It may deliver them in
any pieces; the session reassembles them in its receive buffer. A frame longer
than that buffer is `Oversize`.

`rpc.Loopback` joins two transports back to back over two caller-owned byte
rings. It is what the tests run on.

### Over a message queue

`rpc.QueueTransport` carries the byte stream over two queues of fixed-size
messages, one for each direction. The queue is injected as `rpc.Queue`: a
context pointer, the message size, and a vtable of four functions.

| Function  | Contract                                                     |
|-----------|--------------------------------------------------------------|
| `send`    | Queue one whole message without waiting, or `QueueFull`      |
| `receive` | Take the oldest message; false if the queue is empty         |
| `waiting` | Messages waiting to be received                              |
| `free`    | Messages that can still be sent before the queue is full     |

Nothing here names an operating system. A ThreadX queue, or any other, goes
behind that vtable in the code that owns it.

Each message is packed the same way:

| Offset | Field  | Type  | Meaning                                         |
|-------:|--------|-------|-------------------------------------------------|
| 0      | `used` | `u16` | Bytes in use after this field, little-endian    |
| 2      | bytes  |       | `used` bytes of the stream, then zeroes         |

A write is cut into as many messages as it needs. All of them are full except
possibly the last, which is zero-padded. A message is never sent empty.

- **All or nothing.** A write that needs more messages than the queue has free
  is `LinkFull` and sends none of them. This relies on the transport being the
  only sender on its outgoing queue.
- **A bad length is an error.** A message whose `used` is zero, or more than
  the message has room for, is `BadMessage`. None of its bytes are handed on.
  Bytes from good messages ahead of it are delivered first.
- **Failures stick.** After `BadMessage`, after a queue reports itself gone
  (`LinkDown`), or after a queue refuses part of a write it had room for
  (`LinkDown`), every send and receive returns the same error until the
  transport is reset.

```zig
var tx_message: [message_bytes]u8 = undefined;
var rx_message: [message_bytes]u8 = undefined;
var link = try rpc.QueueTransport.init(out_queue, in_queue, &tx_message, &rx_message);
var client = Client.init(link.transport(), &rx, my_caps);
```

### Over shared memory

`rpc.RingTransport` carries the byte stream over two rings in memory that two
cores share, one ring for each direction. Each ring has one producer and one
consumer. The memory is the caller's: nothing here names an address, and
where the rings live belongs to whoever binds this to a board.

A ring is a header of three 32-byte lines and then the data. Every header
field is a little-endian `u32`, and the numbers are pinned in
`rpc.ring.Layout`.

| Offset | Field      | Written by | Meaning                                    |
|-------:|------------|------------|--------------------------------------------|
| 0      | `magic`    | formatter  | `0x42384152`, the bytes `RA8B`             |
| 4      | `version`  | formatter  | Layout version, 1                          |
| 8      | `capacity` | formatter  | Data bytes in the ring                     |
| 32     | `head`     | producer   | Where the next byte will be written        |
| 64     | `tail`     | consumer   | Where the next byte will be read           |
| 96     | data       | producer   | `capacity` bytes                           |

The rest of each line is zero. `head` and `tail` each have a line to
themselves, so the two cores never write the same one. Both are byte offsets
into the data and are always less than `capacity`. The ring is empty when they
are equal, so one byte is always left unused: a ring holds `capacity - 1`
bytes.

The machine's part is injected as `rpc.Signal`, a context pointer and a vtable
of two functions:

| Function  | Contract                                                         |
|-----------|------------------------------------------------------------------|
| `barrier` | Stores made before the call are visible before stores made after |
| `notify`  | Tell the other core there is something to read; a hint only      |

The order of the steps is the contract between the cores:

- **Writing**: copy the data, `barrier`, publish `head`, `notify`.
- **Reading**: read `head`, `barrier`, copy the data, `barrier`, publish
  `tail`.

So data is in place before the index that reveals it, and a byte is not given
back to the writer until it has been copied out.

- **All or nothing.** A write longer than the free space is `LinkFull`. It
  writes nothing, moves nothing and rings nothing. Data is never overwritten.
- **One doorbell per send.** `notify` is called once for each write that
  carried anything, however it wrapped, and never for one that was refused.
- **The header is checked on every use.** Part of it is written by the other
  core. A wrong `magic`, `version` or `capacity`, or a `head` or `tail` at or
  past `capacity`, is `BadMessage`: nothing is copied and nothing is written.
- **Failures stick.** After `BadMessage`, every send and receive on that
  transport returns it until the transport is reset, even if the header has
  since been mended.

```zig
const up = try rpc.Ring.init(shared[0..ring_bytes]);
const down = try rpc.Ring.init(shared[ring_bytes..][0..ring_bytes]);
up.writeHeader(); // one core only, before the other starts
down.writeHeader();

var link = rpc.RingTransport.init(up, down, signal); // the other core: (down, up)
var client = Client.init(link.transport(), &rx, my_caps);
```

### Resetting a link

Two errors cannot be recovered from in place, because the byte stream has lost
its framing and nothing later on it can be trusted:

- a transport failure that sticks, as above;
- `Oversize` from a session's `poll`: a frame longer than the receive buffer.
  It is refused from its header and stays refused, because the stream cannot
  be read past a frame that was never taken in.

The owner of the link brings it back. With both sides stopped:

1. Empty both queues. `QueueTransport.reset()` empties the queue that end
   receives from, drops the bytes it was holding and clears the failure; call
   it on both ends, or flush the queues directly.
2. Start both sessions again with `Client.init` and `Server.init`. That gives
   each an empty receive buffer and the client an empty pending table.
3. Fail the calls that were in flight. Their waiters will never get a
   response, and the client no longer knows about them.
4. Run the handshake again before any other traffic.

A transport that has no `reset` of its own, such as the loopback, is replaced
with a fresh one at step 1.

For a pair of rings, step 1 is done by the two cores together. Both stop
using the link, by whatever means the binding gives them to agree on that.
One core, the same one that formatted the rings at start-up, calls
`Ring.writeHeader()` on both, which empties them and restores the header. Then
each core calls `RingTransport.reset()` on its own transport to clear the
failure, and both carry on from step 2. A core must not reset its transport
before the headers have been rewritten: the damage would still be there, and
the next call would fail again.

## Tests

`zig build test --summary all` runs fifteen roots:

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
- `queue_transport_test`, `queue_session_test`: the message packing on a
  mocked queue, and a client and a server over two of them: frames split
  across messages, a frame exactly one message long, a full queue, interleaved
  events, a bad length, and the reset after each kind of failure.
- `queue_fuzz_test`: random writes through the packing and back, and random
  messages through the unpacking.
- `ring_transport_test`, `ring_session_test`: one ring's layout, the order of
  its steps and what it refuses, and a client and a server over two rings in
  one shared buffer: wrap-around, a frame that exactly fills the ring, a full
  ring, interleaved events, a corrupted header, indices out of range, the
  doorbell count, and the reset.
- `ring_fuzz_test`: random sends and receives against a plain queue, and a
  header of random words.
- `fuzz_test`: damaged golden frames and random bytes through the decoder,
  then onto the wire of a live session towards each side. The seeded loops run
  every time; `zig build test --fuzz` drives the same properties with coverage
  guidance.

The fixtures are hex text, one field per line, written by hand from the rules
above and not captured from the encoder. They are plain files so that another
implementation of this format can be held to the same bytes.
