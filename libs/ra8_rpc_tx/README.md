# ra8_rpc_tx

ThreadX queues behind `ra8_rpc`. This library implements the queue interface
that `rpc.QueueTransport` takes, `rpc.Queue`, over one ThreadX queue, so a
session in the resident image can run over a pair of them. It is freestanding
Zig with no heap and no libc, and it is the only place the RPC stack names
ThreadX: `ra8_rpc` itself has no ThreadX symbol in it.

This is the resident side only. Code inside a ThreadX module reaches the
kernel through the module dispatcher, not by calling it, and is not covered
here.

## Which ThreadX services

The firmware builds ThreadX with `TX_INCLUDE_USER_DEFINE_FILE`, and
`port/threadx/inc/tx_user.h` does not define `TX_DISABLE_ERROR_CHECKING`. So
in every C file of the resident image `tx_queue_send` is the error-checking
service `_txe_queue_send`, and this binding calls the same three:

| Service               | Used for                                          |
|-----------------------|---------------------------------------------------|
| `_txe_queue_send`     | `send`, with `TX_NO_WAIT`                         |
| `_txe_queue_receive`  | `receive`, with `TX_NO_WAIT`                      |
| `_txe_queue_info_get` | `waiting` and `free`, from the two message counts |

They are named in one file, `src/kernel.zig`. Everything else takes the entry
points as a comptime value, `rpc_tx.Api`, so the host tests pass fakes and
only an image that links the kernel binds the real ones.

## What each ThreadX answer becomes

Nothing here waits. Every call is made with `TX_NO_WAIT`, so it never
suspends the caller.

| ThreadX status        | On `send`   | On `receive`           |
|-----------------------|-------------|------------------------|
| `TX_SUCCESS`          | sent        | one message            |
| `TX_QUEUE_FULL`       | `QueueFull` | fails closed           |
| `TX_QUEUE_EMPTY`      | fails closed | no message yet        |
| anything else         | fails closed | fails closed          |

To fail closed is to return `QueueDown`, keep the ThreadX status in
`failure`, and stay down: every later call returns `QueueDown` without
calling ThreadX again, until `clear`. Nothing is retried and no message is
dropped quietly. If `_txe_queue_info_get` fails, the counts are reported as
nonzero rather than as "empty" or "full", so the caller goes on to the send or
receive that tells it the queue is down.

Through `rpc.QueueTransport` this is what a session sees: a write that needs
more messages than the queue has free is `LinkFull` and sends none of them,
and a queue that is down is `LinkDown`.

## What the caller must get right

- **One sender per queue.** `QueueTransport` counts the free messages and then
  sends. A second sender in between would break that, so each direction of
  each link gets a queue of its own.
- **The message size.** `TxQueue.init` takes the size in bytes and refuses
  anything that is not a multiple of 4 from 4 to 64, which is all a ThreadX
  queue can have. It must be the size the queue was created with: ThreadX
  copies that many bytes on every send and receive and offers no way to check.
  Two of those bytes are the transport's length field, so a message carries 2
  to 62 bytes of the stream.
- **The queue outlives the binding.** A queue deleted underneath it is found
  on the next call and takes the binding down.

```zig
const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const TxQueue = rpc_tx.TxQueue(rpc_tx.kernel.api);

// Two TX_QUEUEs created elsewhere with 16-byte (four-word) messages.
var to_peer = try TxQueue.init(&queue_out, 16);
var from_peer = try TxQueue.init(&queue_in, 16);

var tx_message: [16]u8 = undefined;
var rx_message: [16]u8 = undefined;
var link = try rpc.QueueTransport.init(
    to_peer.queue(),
    from_peer.queue(),
    &tx_message,
    &rx_message,
);
var client = Client.init(link.transport(), &rx, my_caps);
```

The binding copies each message through sixteen aligned words on the stack, so
the transport's byte buffers may sit at any alignment.

## Resetting

A binding that is down stays down. With both sides of the link stopped:

1. Flush both ThreadX queues with `tx_queue_flush`, or delete and recreate
   them if they were the problem.
2. Call `clear` on every `TxQueue` bound to them, on both sides.
3. Carry on with the reset `ra8_rpc` describes for any link: reset both
   `QueueTransport`s, start both sessions again, fail the calls that were in
   flight, and run the handshake.

`failure` holds the ThreadX status that caused it until `clear`.

## Tests

`zig build test --summary all` runs two roots on the host, against fake
ThreadX services in `tests/fake_threadx.zig` that make the same checks the
`_txe_` services do:

- `queue_test`: send, receive, the counts, a full queue, an empty queue, each
  unexpected status and that it is not retried, a deleted queue, the message
  sizes refused, and that ThreadX is always handed aligned words and
  `TX_NO_WAIT`.
- `session_test`: a client and a server over two ThreadX queues through
  `rpc.QueueTransport`: a frame split across several queue messages, a full
  queue refusing a frame whole, interleaved events, and an unexpected status
  through to the session and back after a reset.
