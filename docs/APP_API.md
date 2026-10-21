# The module call table (`ra8_app_api_t`)

The calls a loaded ThreadX module makes into the resident image: drawing in
its screen region, reading its input, using files under its own directory, and
its lifecycle. The code is `libs/ra8_app/src/internal/api/`; the container a
module arrives in is [`.ra8app`](formats/RA8A.md).

## How a call travels

```
module thread                     ThreadX                     resident image
-------------                     -------                     --------------
Api.drawRect(rect, argb)
  -> txm_module_application_request(6, &rect, argb, 0)
                                  SVC, + TXM_APPLICATION_REQUEST_ID_BASE
                                  Module Manager kernel dispatch
                                  - TXM_APPLICATION_REQUEST_ID_BASE
                                    -> _txm_module_manager_application_request(6, ...)
                                                              kernel.dispatch(backend, caller, 6, ...)
                                                                checks, then backend.fill(...)
  <- status word ---------------------------------------------
```

No kernel function pointer crosses into the module. The module only packs
words and traps; every check runs in the resident image against the calling
module, so a module never runs kernel code with its own MPU view.

Two sides, one ABI:

- **Module side** (`api/module.zig`): `Api(entry)` is the typed surface. A
  module binds `Api(module.target)`, which calls ThreadX's
  `_txm_module_application_request`. Each method returns a Zig error from
  `module.Error` on a non-`ok` status.
- **Resident side** (`api/kernel.zig`): `kernel.dispatch(backend, caller,
  request, p1, p2, p3)` returns the status word. The image's
  `_txm_module_manager_application_request` builds the `Caller` from the
  calling module instance (`txm_module_instance_data_start` to `_data_end`,
  the shared region if any) and the capabilities of its admitted `.ra8app`
  header, and passes its display, file and input services as the `Backend`.

## The requests

Numbers are what the module passes to `txm_module_application_request`, below
`TXM_APPLICATION_REQUEST_ID_BASE`. They are append-only: a number is never
reused or renumbered, and zero is never a request.

| # | Request | `p1` | `p2` | `p3` | Needs | Answer |
|--:|---------|------|------|------|-------|--------|
| 1 | `api_version` | | | | | the generation, not a status |
| 2 | `exit` | code | | | | the host stops and unloads the module |
| 3 | `ticks` | `*u32` out | | | | ms since the module started, wrapping |
| 4 | `sleep` | ms | | | | suspends the calling thread |
| 5 | `screen_info` | `*ScreenInfo` out | | | display | the module's region |
| 6 | `draw_rect` | `*const Rect` | ARGB | | display | fills a rectangle |
| 7 | `draw_image` | `*const Rect` | pixels | bytes | display | ARGB8888 rows, `w * h * 4` bytes |
| 8 | `file_open` | `*FileOpen` | | | storage | writes `handle` back |
| 9 | `file_read` | `*FileIo` | | | storage | writes `done` back |
| 10 | `file_write` | `*FileIo` | | | storage | writes `done` back |
| 11 | `file_close` | handle | | | storage | |
| 12 | `touch_event` | `*TouchEvent` out | | | display | `no_event` when the queue is empty |

Input needs `display`: a touch belongs to the screen region it lands on, and a
module without one has no input.

### Argument structs

All `extern`, little-endian, in module memory. `Word` is `ALIGN_TYPE`, 32 bits
on the Cortex-M.

| Struct | Fields (offset on the M85) |
|--------|----------------------------|
| `Rect` | `x u16 @0`, `y u16 @2`, `w u16 @4`, `h u16 @6` |
| `ScreenInfo` | `width u16 @0`, `height u16 @2`, `bytes_per_pixel u16 @4` (4), `reserved u16 @6` |
| `FileOpen` | `path Word @0`, `path_len u32 @4`, `flags u32 @8`, `handle u32 @12` (out) |
| `FileIo` | `handle u32 @0`, `buffer Word @4`, `len u32 @8`, `done u32 @12` (out) |
| `TouchEvent` | `kind u16 @0`, `x u16 @2`, `y u16 @4`, `button u16 @6` |

Open flags: `read 0x1`, `write 0x2`, `create 0x4` (with `write`: create if
missing, truncate if not). At least one of `read` and `write`; any other bit is
refused. `TouchEvent.kind`: `press 1`, `move 2`, `release 3`, `button_down 4`,
`button_up 5`.

### Status words

| Word | Status | Module error |
|-----:|--------|--------------|
| 0 | `ok` | |
| 1 | `unknown_request` | `UnknownRequest` |
| 2 | `denied` | `Denied` |
| 3 | `bad_pointer` | `BadPointer` |
| 4 | `bad_argument` | `BadArgument` |
| 5 | `not_found` | `NotFound` |
| 6 | `io_error` | `IoError` |
| 7 | `no_resource` | `NoResource` |
| 8 | `no_event` | (`touchEvent` returns null) |

## What the resident side checks

`dispatch` refuses in this order and stops at the first refusal. A refusal
never reaches the backend.

1. **Unknown request.** Anything outside `1 ..= 12`.
2. **Capability.** The request's capability must be in the caller's admitted
   manifest. This is checked before any pointer is read.
3. **Pointers.** Every pointer, and every buffer an argument block names, must
   lie wholly in the module's data region or its shared region: not null, not
   wrapping, aligned for its type. This is ThreadX's
   `TXM_MODULE_MANAGER_CHECK_INSIDE_DATA` rule. Argument blocks are copied out
   of module memory before they are checked, so another module thread cannot
   change one between the check and the use; out values are written back
   after the backend answers.
4. **Arguments.** A rectangle must be non-empty and inside the region the
   backend reports; `draw_image`'s length must be exactly `w * h *
   bytes_per_pixel`; a word wider than its 32-bit argument is refused.
5. **Paths.** A `file_open` path is 1 to 96 bytes of printable ASCII,
   components joined by single `/`, none of them `.`, `..` or empty, and no
   `\` or `:`. The backend receives `/apps/<app_id>/<path>`, with `app_id`
   from the admitted header, so a module cannot name a file outside its own
   directory.

## Versioning

`abi.version` is the generation the firmware serves, and it is the same
number as the `.ra8app` header's `api_version_current` (a test holds them
equal). A `.ra8app` whose `min_api_version` is higher is refused at load, so a
module never calls a number this firmware does not serve.

To add a call: append a request number, bump `version` and
`api_version_current` together, and give it a row above. To change what a
call means, add a new number; the old one keeps its meaning for modules built
against it.

## Status

The table, the dispatch and the shims are host-tested
(`libs/ra8_app/tests/app_api_test.zig`, `app_api_refusal_test.zig`). No
resident image registers `_txm_module_manager_application_request` yet; the
first app to use the table is RA8FW-1023's syscall tour.
