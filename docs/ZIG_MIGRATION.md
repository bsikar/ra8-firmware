# Zig Library Migration

## Purpose

This document defines the physical boundary for a first-party library migrated
from C to Zig. It keeps the repository's consumers in C, makes the C ABI
deliberate, and gives native Zig implementation code a clean place to grow.

This is a layout convention. The [ABI representation contract](ZIG_ABI.md)
defines scalar and aggregate representation plus error and output behavior.
It also defines ownership, buffers, opaque handles, callbacks, traps, and
concurrency; every applicable rule is settled before a migrated library exposes
its ABI.

## Library layout

Each migrated first-party library uses this shape. Existing library names stay
unchanged unless a separate migration decision says otherwise.

```text
libs/<library>/
  inc/
    <library>.h              Public, C-compatible ABI contract
  src/
    <library>_abi.zig        Only C ABI export membrane
    internal/
      root.zig               Native Zig implementation entry point
      ...                    Native Zig implementation modules
  build.zig                  Library-local Zig build root
  CMakeLists.txt             Transitional CMake integration, when needed
tests/<area>/
  src/
    test_<library>.c         C acceptance tests using the public header
```

The public header below a library's `inc/` directory is the stable source-level
contract consumed by C, Rust FFI bindings, examples, and ABI acceptance tests.
The ABI adapter below `src/` is the single implementation membrane that
translates that contract into native Zig. The internal implementation directory
is private implementation detail and may use ordinary Zig types, modules, and
refactoring patterns.

The initial CMake integration remains only a transitional caller while the
repository moves its build graph to Zig. `build.zig` is still required for
every library that contains Zig so Zig's build and test gates have an explicit
root.

## Dependency direction

```text
C tests, C applications, Rust FFI bindings
                 |
                 v
  libs/<library>/inc/<library>.h
                 |
                 v
       ABI adapter module
                 |
                 v
   native Zig implementation
```

Only the header is public to non-Zig consumers. Only the adapter may export
the library's C ABI. Internal modules must not add `export` declarations, be
added to public include paths, or be named by generated public headers.

## Public-header rules

The public header is ordinary repository-owned C23 source. It is hand-written
and reviewed as an ABI contract; Zig must not generate it implicitly during a
normal library build.

- Keep it below `libs/<library>/inc/` and give it the library's established
  name.
- Include only standard C headers and intentionally public project headers.
  Never include a `src/` header, a Zig source file, or a generated private
  artifact.
- Declare only C-compatible names, constants, types, and functions. Do not
  expose Zig module names, allocator types, error unions, slices, or pointers
  to internal Zig structures.
- Wrap declarations with `extern "C"` guards so C++ consumers retain C
  linkage. The repository's C and Rust consumers use the same C ABI.
- Treat a change to a declaration, its referenced public type, or required
  include as an ABI review event. Do not rely on a matching Zig declaration
  being inferred from the header.

Minimal header template:

```c
/**
 * @brief C ABI for the ra8_demo library.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

uint32_t ra8_demo_add(uint32_t left, uint32_t right);

#ifdef __cplusplus
}
#endif
```

The example intentionally uses only fixed-width scalar values. The ABI
representation contract decides how enumerations, structures, booleans,
handles, and other ABI-visible values are declared.

## Zig adapter rules

The ABI adapter owns every C-linkage symbol implemented by the library. It
imports internal Zig modules and performs only the boundary work: validating or
converting public inputs as required by the ABI policy, invoking the
implementation, and converting the result back to the public contract.

Minimal adapter template:

```zig
const implementation = @import("internal/root.zig");

pub export fn ra8_demo_add(left: u32, right: u32) callconv(.c) u32 {
    return implementation.add(left, right);
}
```

The corresponding internal module has no C exports:

```zig
pub fn add(left: u32, right: u32) u32 {
    return left + right;
}
```

Do not use `@cImport` to make the public header the implementation's type
model. If an adapter needs a declaration from a C dependency, keep that import
private to the adapter or another private interoperability module. Native Zig
modules communicate with native Zig types; the adapter owns their conversion
to and from the C ABI.

## Visibility and generated artifacts

- `pub export` is permitted only in `<library>_abi.zig`. A library build must
  not accidentally export internal helper symbols.
- The adapter's exported symbol set and the function declarations in the
  library's public header must be one-to-one. The ABI harness introduced by
  the foundation epic will verify this rather than trusting review alone.
- Generated files are private by default. A generated public header is
  allowed only when a later issue defines its checked-in source of truth,
  deterministic generation command, review path, and ABI verification.
- Generated register headers and other device artifacts do not define a
  library's public Zig ABI merely because a migrated library uses them.
- Public headers must not expose paths into `src/`, build output directories,
  `.zig-cache`, or any generated private directory.

## C acceptance tests

The first acceptance test for a migrated library is a C test placed in the
appropriate existing test-area `src/` directory. It includes only the public
header, calls the exported symbols, and links the produced library artifact.
It must not include private headers, compile an internal Zig source directly,
or reach implementation-only symbols.

This test proves the boundary from the consumer side. Zig unit tests remain
valuable for internal behavior, but they do not replace the C compile, link,
and runtime acceptance path.

## Zig native tests

Every first-party Zig build root provides an explicit `zig build test` step
that runs Zig `test` declarations through `std.testing`. Put private-behavior
tests beside the native module they exercise and adapter-mapping tests beside
the ABI adapter. These tests may call native Zig interfaces directly; they are
separate evidence from C acceptance tests at the exported boundary.

Each build root also carries `.zig-test-contract.json`. Its `test_roots` list
names the source modules compiled by the build graph's test step,
`covered_sources` inventories every other source compiled and tested directly,
and `minimum_tests` records the reviewed non-vacuity floor. The gate checks
Zig's verbose build evidence to prove the declared roots actually reached the
compiler; textual imports or comments are not reachability evidence. It also
runs each covered source through `zig test`, then rejects a missing test step,
an unlisted source, or an executed-test count below the floor.

Adding or removing a Zig source therefore requires wiring it into a declared
test root. Deliberately lowering a test floor is a review event and must be
justified by the same change that removes or consolidates the tests.

## Migration checklist for this boundary

1. Retain or create the public header below `libs/<library>/inc/` as the C ABI
   contract.
2. Add the one C-export membrane below `libs/<library>/src/`.
3. Move native implementation modules into the internal implementation
   directory without C exports.
4. Add the library-local `build.zig` root and wire it into the transitional
   build graph.
5. Add a C acceptance test that uses only the public header and artifact.
6. Add native Zig tests and update the build root's test contract.
7. Apply the foundation policies for representation, failures, ownership,
   callbacks, traps, and concurrency before exposing those categories. Every
   export declares its calling-context classification; reject an implicit
   host-thread assumption, an unclassified export, or an ISR path that reaches
   a task-only API.

The reusable ABI harness, policy enforcement, and migration review checklist
are deliberately follow-on work. This document makes their physical targets
unambiguous.
