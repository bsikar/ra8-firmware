# Zig C ABI Representation Contract

## Scope

This contract governs every first-party Zig library's public C ABI. The public
C header remains the consumer-facing declaration, and the Zig ABI adapter
implements that declaration. Native Zig types are not public ABI types.

The contract deliberately optimizes for a single, auditable representation on
the host and the 32-bit RA8 target. A convenient target-local representation is
not allowed at the boundary when it would make width, alignment, signedness, or
calling behavior depend on the consumer's architecture.

This document covers representation and error reporting. Ownership, callbacks,
panic containment, and concurrency have separate contracts.

## Error and output contract

Every exported C ABI function that can fail returns `ra8_err_t` from
`libs/ra8_core/inc/ra8_err.h`. `k_ra8_ok` is the only success value. A Zig
adapter maps every recoverable implementation failure to one documented
`k_ra8_err_*` value; it never exports a Zig error union, error-set member,
optional-as-status convention, or panic representation.

An exported function that cannot fail may return `void` only when it has no
input validation, allocation or capacity dependency, external operation, or
observable failure mode. If any such condition can arise, it returns
`ra8_err_t`. A function's return value is its sole mandatory error channel:
there is no ABI-visible `errno`, last-error global, thread-local error, or
diagnostic string that callers must inspect.

### Mapping Zig failures to `ra8_err_t`

The adapter owns a closed, reviewed mapping from internal Zig failures to the
existing public vocabulary. Map to the most specific existing code; do not
expose a library-private integer or collapse a known cause into `k_ra8_fail`.

| Failure class | Public result | Retry rule |
| --- | --- | --- |
| Required pointer is absent | `k_ra8_err_null_ptr` | Not retryable until the caller supplies it. |
| Length, capacity, alignment, or encoded-size limit is invalid | `k_ra8_err_invalid_size` | Not retryable until corrected. |
| Other malformed or inconsistent input | `k_ra8_err_invalid_arg` | Not retryable until corrected. |
| Unsupported feature or configuration | `k_ra8_err_not_supported` | Not retryable without a different capability or build. |
| Fixed pool or bounded workspace exhausted | `k_ra8_err_no_mem` | Retry only after capacity is released or the input changes. |
| Resource is busy, data is absent, or a nonblocking action would block | `k_ra8_err_busy`, `k_ra8_err_no_data`, or `k_ra8_err_would_block` | Retry after the documented external condition changes. |
| Device is not ready | `k_ra8_err_hw_not_ready` | Retry only after the documented readiness transition. |
| Timeout | `k_ra8_err_timeout` or `k_ra8_err_hw_timeout` | Retryability and side effects must be stated by the API. |
| Known transport, validation, hardware, or state failure | The matching specific `k_ra8_err_*` value | Stated by the API; do not infer safety from the category. |
| Reviewed internal failure with no public equivalent | `k_ra8_fail` | Not retryable unless the API explicitly says otherwise. |

Adding a genuinely new public error needs an explicit numeric member in
`ra8_err_t`, documentation, and ABI review. It is not acceptable to cast a
Zig error ordinal, use an unrecognized numeric value, or add a library-local
status enumeration at the C boundary.

### Validation order and no-write default

The adapter validates the entire public argument tuple before invoking the
implementation or writing an output. It uses this fixed precedence:

1. Required output pointers are checked first.
2. Other required pointers are checked second.
3. Length, capacity, alignment, and range checks follow.
4. Cross-argument semantic checks follow.
5. Only then may the implementation run and publish outputs.

For every non-success result, the default is **no published output**: scalar
and aggregate output objects retain their incoming bytes, output counts retain
their incoming values, and output buffers are not modified. The adapter must
compute into native temporary state and copy to caller storage only after the
operation succeeds. This lets C tests initialize outputs to sentinels and
assert each failure path without interpreting Zig state.

An API that cannot meet this transactional default must be explicitly named
and documented as a partial-result operation. Its public header identifies
which output prefix is valid on each non-success code, sets an output count to
the exact valid prefix length, and describes input consumption and retry
semantics. A partial-result API is never inferred from an ordinary
pointer-and-length signature.

### Pointer, length, and output combinations

For an ordinary input byte span, a null data pointer is permitted only when its
length is zero; it denotes an empty span and is never dereferenced. A nonzero
length with a null data pointer returns `k_ra8_err_null_ptr`. A length beyond
the documented maximum returns `k_ra8_err_invalid_size`, with no output write.

An ordinary scalar or aggregate output pointer is always required. A null
output pointer returns `k_ra8_err_null_ptr` before any other argument is
examined, and all other outputs remain unchanged. Do not use a null output
pointer as an undocumented request to discard a result.

For an output byte span, a non-null destination is required whenever capacity
is nonzero. A null destination with zero capacity is permitted only for a
header-documented sizing-query form that also supplies a required output-size
pointer; otherwise it returns `k_ra8_err_null_ptr`. Insufficient capacity
returns `k_ra8_err_invalid_size` and writes neither the destination nor any
output count under the no-write default.

The header documents any intentional exception to these rules in the function
contract, including the exact validation precedence. An exception still uses a
public `ra8_err_t` result and must be covered by C acceptance tests.

### Retryability, side effects, and diagnostics

An error code alone does not promise that replaying a call is safe. Each
fallible function documents, for every non-success code, whether it performed
no side effect, may have changed internal state, may be retried immediately,
or requires an external event, reset, or new input. `k_ra8_err_cancelled`
retains its existing atomic-cancellation meaning: no public operation effect
was committed.

Expected caller mistakes and ordinary recoverable conditions are reported by
the return code and are not automatically logged by the Zig adapter. Logging a
recoverable error requires an actionable event, rate policy, and ownership in
the library contract; the log must use stable public diagnostics rather than
an internal Zig error name. Fatal traps and panic containment are handled by
the separate panic-boundary contract, not represented as `ra8_err_t` by
default.

### Adapter rule

The adapter performs the public validation and maps only reviewed internal
failure cases. It publishes each output after success and returns
`k_ra8_ok` last. Native Zig modules may use error unions internally, but those
types stop at the adapter. The reusable ABI harness will compile C callers
that exercise every mapped failure, output sentinel, and pointer-length case.

## Ownership, buffers, and opaque handles

Every exported function names the owner of each resource before and after the
call. Ownership is never inferred from `const`, a pointer spelling, or a Zig
implementation detail. The public ABI does not expose a Zig allocator, slice,
array-list, error payload, or pointer to native Zig state.

### Buffer vocabulary

An API uses one of these terms in its C header and documentation:

| Term | Contract |
| --- | --- |
| **Borrowed input** | Caller retains ownership for the duration of the call; the library does not retain the pointer. |
| **Copied input** | Caller retains ownership; the library has copied all required bytes before success returns. |
| **Caller-owned output** | Caller owns the destination storage; the library writes it only under the error contract and does not retain it. |
| **Library-owned output** | The library returns a pointer that must be released by its named matching release function. |
| **Retained input** | The library stores caller memory after return; prohibited unless the callback/retention contract explicitly authorizes it. |

Ordinary input spans are borrowed. They pair a fixed-width pointer and length,
are valid only for the call, and are never stored by the adapter or an internal
module. A library that needs bytes after return copies them into storage that it
owns; a successful copy transfers no ownership from the caller. Strings are
byte spans with an explicitly documented encoding and length; NUL termination
is not assumed unless the API says so.

Capacity and length use the fixed-width representation contract. A
library-owned variable-length result uses `uint8_t** out_bytes` and
`uint32_t* out_len`; it publishes both only on success. Text uses the same
byte form with a documented encoding. Its library supplies one named
`ra8_err_t ra8_<library>_bytes_release(uint8_t** in_out_bytes)` function that
releases the allocation and sets the caller pointer to null on success. The
associated returned length is invalid only after a successful release. The
caller does not call `free`, a Zig allocator, or a generic release helper.
`in_out_bytes == NULL` returns `k_ra8_err_null_ptr` because the adapter cannot
clear it. `*in_out_bytes == NULL` is the optional idempotent no-op case and
must be stated by the header; otherwise it returns `k_ra8_err_null_ptr`.

### Opaque handles

State crossing the ABI is an incomplete C type used only behind a pointer. Its
definition remains private to Zig; the C header never publishes its size,
fields, alignment, or allocation strategy.

```c
typedef struct ra8_sample ra8_sample_t;

ra8_err_t ra8_sample_create(ra8_sample_t** out_handle);
ra8_err_t ra8_sample_destroy(ra8_sample_t** in_out_handle);
```

Creation allocates or binds all library-owned state only after validation. On
failure, it publishes no handle and leaves the caller's output unchanged under
the error contract. On success, the caller owns one handle reference and must
use the named destroy or deinit function exactly once for that reference.

Destroy and deinit take an in-out handle pointer when they can invalidate it.
On successful destruction they release all library-owned resources and set the
caller's handle to null. Repeated destruction of that null value succeeds as a
no-op. Only a non-null handle returned by the matching create function and not
yet destroyed is valid. A copied alias after destruction, a fabricated pointer,
or a handle of another type is a caller contract violation; the adapter must
not promise to recognize it before dereferencing it. Borrowed handles are
never destroyed by the borrower.

`init`/`deinit` is reserved for caller-provided storage whose size and layout
are themselves public and stable. A Zig-backed opaque type therefore normally
uses `create`/`destroy`; it must not make callers allocate guessed storage for
private Zig state.

### Transfer, retention, and cleanup

The initial Zig ABI forbids ownership transfer of caller-owned raw buffers.
An API needing a transfer uses a named opaque handle or a library-owned copy
instead. It also forbids implicit pointer retention. A future retained-pointer
API must name its retain and release boundary, duration, cancellation path,
threading context, and teardown behavior before it is exposed.

Every allocating or multi-step creation path has one cleanup path for each
failure point. Allocation exhaustion maps to `k_ra8_err_no_mem`; it leaks no
resource, publishes no partial handle, and does not consume a caller-owned
buffer. Each allocating library supplies a private host-test allocation seam
that can fail every allocation point deterministically; it is not part of the
public ABI. C contract tests use that seam to prove no leak, untouched outputs,
and a later successful create/destroy. They also test null and cleared-handle
repeated teardown without inspecting Zig state; arbitrary pointer misuse is
not a safe C test vector.

## Callbacks, retention, and reentrancy

Callbacks are prohibited by default. A library may accept one only through a
named registration API whose header declares a fixed C function-pointer type,
an explicit `void*` context, callback return meaning, calling context, and
unregistration function. The adapter passes no Zig closure, slice, allocator,
or borrowed internal pointer to C.

Registration borrows the callback and context; the caller retains ownership.
The caller keeps both valid until successful unregister or destroy returns.
Unregister is the named release boundary: after it succeeds, no callback may
start or continue, and it waits for any active callback to finish.
Registration, unregistration, and destruction are not reentrant from the
callback unless the header explicitly authorizes that exact operation. A
callback may not call any API on the same handle except functions explicitly
documented as reentrant.

Retaining any other caller pointer after return is prohibited unless the API
uses the same explicit register/unregister lifecycle. The header states who
cancels outstanding work and which error is returned if cancellation cannot
complete. Destroy performs an
implicit unregister before releasing state, or returns a documented error and
leaves the handle live; it never calls a callback after successful destruction.

Callbacks run only in ordinary task/caller context. ISR-context callbacks,
callbacks while locks are held, and callbacks during teardown are prohibited
until a later dedicated contract authorizes them. C contract tests cover a
successful callback, a callback-reported error, cancellation, unregister while
idle, and destruction ordering.

## Allowed scalar values

Public value parameters, return values, and structure fields may use only the
following fixed-width pairs. The C spelling is used in the public header and
the Zig spelling is used in the adapter's boundary type.

| C declaration | Zig declaration | Width | Use |
| --- | --- | --- | --- |
| `uint8_t` | `u8` | 8 bits | Unsigned byte and canonical boolean. |
| `uint16_t` | `u16` | 16 bits | Unsigned scalar. |
| `uint32_t` | `u32` | 32 bits | Unsigned scalar, count, or length. |
| `uint64_t` | `u64` | 64 bits | Unsigned scalar. |
| `int8_t` | `i8` | 8 bits | Signed scalar. |
| `int16_t` | `i16` | 16 bits | Signed scalar. |
| `int32_t` | `i32` | 32 bits | Signed scalar. |
| `int64_t` | `i64` | 64 bits | Signed scalar. |

`uint8_t` is the only public boolean representation. Its valid values are
zero and one; the adapter validates externally supplied values before treating
them as a native Zig `bool`. Do not place C `bool`, Zig `bool`, `char`,
`wchar_t`, `short`, `int`, `long`, `long long`, `size_t`, `ptrdiff_t`, or a
floating-point type in the public ABI.

`size_t` and pointer-difference values are specifically prohibited even for
byte counts: they are 64-bit on common hosts and 32-bit on the RA8 target. Use
the narrowest fixed-width unsigned type that the API's documented maximum
permits. A length does not become a pointer-sized value merely because it
describes a buffer.

## Enumerations

Every public enumeration has an explicit fixed underlying unsigned type in C
and the same explicit integer tag in Zig. The underlying width is selected
from the scalar table, and every enumerator has an explicit numeric value.

```c
typedef enum : uint8_t {
  k_ra8_sample_mode_idle = 0U,
  k_ra8_sample_mode_run  = 1U,
} ra8_sample_mode_t;

static_assert(sizeof(ra8_sample_mode_t) == 1U, "ABI enum width");
static_assert(k_ra8_sample_mode_idle == 0U, "ABI enum value");
static_assert(k_ra8_sample_mode_run == 1U, "ABI enum value");
```

```zig
const SampleMode = enum(u8) {
    idle = 0,
    run = 1,
};

comptime {
    if (@sizeOf(SampleMode) != 1) @compileError("ABI enum width");
    if (@intFromEnum(SampleMode.idle) != 0) @compileError("ABI enum value");
    if (@intFromEnum(SampleMode.run) != 1) @compileError("ABI enum value");
}
```

Do not use a C enum with implementation-selected width, a Zig enum without an
explicit integer tag, enum bitfields, or an enum to carry flags unless its
individual bit values and allowed combinations are documented. Adding or
renumbering an enumeration value is an ABI change.

## Structures, unions, padding, and alignment

A public aggregate is allowed only when it has a stable, target-neutral layout
that is asserted in both languages. Its C form is a `typedef struct` with
fixed-width fields; its Zig counterpart is an `extern struct` with the same
field order and scalar representations. A public union follows the same rule
using C `union` and Zig `extern union`, but requires an explicit discriminator
in the surrounding API; untagged union interpretation is forbidden.

```c
typedef struct {
  uint32_t count;
  uint16_t mode;
  uint8_t enabled;
  uint8_t reserved0;
} ra8_sample_config_t;

static_assert(sizeof(ra8_sample_config_t) == 8U, "ABI structure size");
static_assert(alignof(ra8_sample_config_t) == 4U, "ABI structure alignment");
static_assert(offsetof(ra8_sample_config_t, count) == 0U, "ABI count offset");
static_assert(offsetof(ra8_sample_config_t, mode) == 4U, "ABI mode offset");
static_assert(offsetof(ra8_sample_config_t, enabled) == 6U, "ABI enabled offset");
static_assert(offsetof(ra8_sample_config_t, reserved0) == 7U, "ABI reserved offset");
```

```zig
const SampleConfig = extern struct {
    count: u32,
    mode: u16,
    enabled: u8,
    reserved0: u8,
};

comptime {
    if (@sizeOf(SampleConfig) != 8) @compileError("ABI structure size");
    if (@alignOf(SampleConfig) != 4) @compileError("ABI structure alignment");
    if (@offsetOf(SampleConfig, "count") != 0) @compileError("ABI count offset");
    if (@offsetOf(SampleConfig, "mode") != 4) @compileError("ABI mode offset");
    if (@offsetOf(SampleConfig, "enabled") != 6) @compileError("ABI enabled offset");
    if (@offsetOf(SampleConfig, "reserved0") != 7) @compileError("ABI reserved offset");
}
```

Every public aggregate must assert its total size, alignment, and every field
offset in both the public C header and the Zig ABI adapter. The standard C
definition supplies `offsetof`; C23 supplies `alignof` and `static_assert`
directly. These
assertions use numeric constants, not expressions derived from a second copy
of the structure.

Padding must be explicit named `uint8_t` reserved fields, included in the
assertions, initialized by the producer, and ignored by the consumer unless a
later ABI version assigns it a documented meaning. Compiler-inserted padding,
bitfields, `#pragma pack`, implementation attributes such as `packed`, and
Zig `packed struct` are forbidden in public ABI aggregates. They are not a
safe substitute for a serialized wire format.

Flexible array members, variable-length arrays, incomplete by-value types,
and pointer fields are forbidden in public aggregates. If an API needs
variable data or indirection, use a separately specified pointer-and-length
operation or an opaque handle under the ownership contract.

## Pointers, addresses, and byte order

Pointers are permitted only as function parameters or returns, never as fields
in public aggregates. They are target-sized and therefore cannot be part of a
host/RA8-identical value layout, serialized data, persistent state, register
image, or wire protocol.

The only initial pointer forms are pointers to fixed-width byte data and
pointers to a forward-declared opaque C type. Nullability, lifetime,
mutability, buffer lengths, and ownership are not implied by the type spelling
and must be specified by the ownership contract before the API is exposed.
Function pointers are callbacks and wait for the callback contract.

`uintptr_t`, `intptr_t`, casts between pointers and integers, and pointers to
native Zig structures are prohibited in the public ABI. A hardware address,
offset, token, or protocol value must use a fixed-width integer with explicit
units and range. A C ABI value is host-native in byte order; it is never a
wire-format declaration. Serialized and device-register byte order must be
converted explicitly at its protocol or register boundary.

## Versioning

An exported C function name, parameter list, return representation, enum tag,
enumerator value, public aggregate size, alignment, or field offset is
immutable once released. Do not extend a public aggregate in place, including
by consuming a reserved byte. Add a new explicitly named type and function
instead, then retain the old entry point for its supported compatibility
window.

Reserved fields exist to make padding observable and deterministic, not as an
unannounced extension mechanism. Any reuse is a new ABI version and requires a
new type or an independently versioned protocol contract.

## Required compile-time evidence

For every public ABI declaration, the owning library must keep both sets of
assertions shown above:

1. The C header checks fixed-width scalar assumptions, enum width and values,
   and aggregate size, alignment, and offsets.
2. The Zig adapter checks the matching Zig types with `@sizeOf`, `@alignOf`,
   `@offsetOf`, and explicit enum-value checks in a `comptime` block.

Both the host build and the RA8 target build must compile those assertions.
The required constants must be identical across those builds for all scalar,
enum, and aggregate values. Pointer-only APIs are checked for their declared
form on each target but are never used to justify target-dependent aggregate
layouts.

The reusable C compile/link/layout harness and policy scanner will enforce
these requirements in follow-on issues. Until they land, a migration review
must reject any ABI declaration without this evidence.
