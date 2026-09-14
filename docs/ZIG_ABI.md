# Zig C ABI Representation Contract

## Scope

This contract governs every first-party Zig library's public C ABI. The public
C header remains the consumer-facing declaration, and the Zig ABI adapter
implements that declaration. Native Zig types are not public ABI types.

The contract deliberately optimizes for a single, auditable representation on
the host and the 32-bit RA8 target. A convenient target-local representation is
not allowed at the boundary when it would make width, alignment, signedness, or
calling behavior depend on the consumer's architecture.

This document covers representation only. Error reporting, ownership,
callbacks, panic containment, and concurrency have separate contracts.

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
