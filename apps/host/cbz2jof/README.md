<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# cbz2jof

A host CLI that converts a CBZ archive into the repository's JOF page-atlas
format, one worker invocation per image entry. The Go layer owns archive
policy; the `jof-worker` C binary owns pixels, running the firmware's own
`jof_produce()` pipeline so a page converted here is byte-identical to one
the RA8 would produce from the same source bytes.

```text
cbz2jof [--worker PATH] input.cbz output-directory
```

The worker resolves from `--worker`, then `CBZ2JOF_WORKER`, then an
executable `jof-worker` beside the CLI. Explicit and environment paths must
contain a separator, so neither can smuggle in a `PATH` lookup.

## What `--worker` is (and when you need it)

The Go CLI never touches pixels itself. For each image it shells out to a
separate C binary, `jof-worker` (`inc/jof_worker.h`, `src/jof_worker.c`),
which drives the firmware's own `jof_produce()` pipeline under the codebase's
embedded-grade constraints (bounded POSIX I/O, `RA8_NASA_RULE_3_OK`-annotated
arenas, no `FILE *`). It is a separate process -- rather than cgo -- so the Go
side stays dependency-free and the C side keeps those constraints intact.

You normally never pass `--worker` by hand: CMake stages `jof-worker` beside
the CLI in the same build directory, so the sibling fallback just works.

```sh
just apps::host::build cbz2jof                # builds both, stages worker beside CLI
just apps::host::run cbz2jof "book.cbz out-dir"

cbz2jof book.cbz out-dir                       # sibling jof-worker resolves automatically
cbz2jof --worker /abs/path/jof-worker book.cbz out-dir   # explicit path
CBZ2JOF_WORKER=/abs/path/jof-worker cbz2jof book.cbz out-dir
```

## Where the code lives

Each language has its own subtree, Go-idiomatic inside its module root and C
following the `mdl`/firmware `inc`+`src` split:

- `go/cbz2jof.go` -- reusable conversion package: argument grammar, worker
  resolution, entry selection and ordering, limits, atomic publication.
- `go/cmd/cbz2jof/main.go` -- CLI process entry; sibling lookup via
  `os.Executable`.
- `go/tests/` -- Go unit tests plus the tagged `realworker` integration test.
- `go/go.mod` -- the Go module root (no external dependencies).
- `inc/jof_worker.h`, `src/jof_worker.c`, `src/jof_worker_main.c` -- the
  per-page C worker and its entry point.

The Go module lives under `go/` rather than sharing the package root because
the Go tool rejects a directory mixing Go and C sources without cgo; keeping
the two languages in separate subtrees preserves each one's own convention
(the C worker keeps the firmware `inc/`+`src/` split, the Go code keeps the
module-root-plus-`cmd/` layout).

## Behavior

Only regular `.jpg`, `.jpeg`, `.png`, and `.webp` entries convert, matched
case-insensitively and ordered by byte-wise filename, then ZIP index.
Each successful page publishes atomically as `page-0001.jof`,
`page-0002.jof`, and so on through a temporary file plus rename; a failed
page leaves no trace but earlier pages survive it. Reruns replace only the
pages they convert; stale higher-numbered pages are kept, never deleted.

Named limits, refused rather than truncated: 256 MiB input CBZ, 65,536 ZIP
entries, 1,024-byte entry names, 65,536 selected images, 256 MiB per
extracted image, 256-pixel worker tile height.
