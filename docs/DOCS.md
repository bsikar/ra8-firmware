# Doxygen HTML Documentation

This document explains how to generate, browse, and maintain the
project's API reference, which is produced by Doxygen from the
in-tree source comments. The configuration file is the top-level
`Doxyfile`; the wrapper script is `scripts/build_docs.sh`; the
canonical entry point is `make docs`.

## Generating the HTML

From the repository root:

```sh
make docs               # build into build/docs/html/
make docs && open build/docs/html/index.html   # macOS
make docs && xdg-open build/docs/html/index.html  # Linux
```

`make docs` is a thin wrapper around `bash scripts/build_docs.sh`,
which in turn:

1. Verifies that `doxygen` is on the PATH.
2. Detects whether `dot` (Graphviz) is also available. When `dot`
   is missing the script overrides `HAVE_DOT=NO` so the build still
   succeeds, just without call/caller graphs.
3. Runs `doxygen Doxyfile`. Output lands in `build/docs/html/`;
   warnings are appended to `build/docs/doxygen-warnings.log`.

To open the freshly-built HTML in a browser without a separate
command, pass `--open`:

```sh
bash scripts/build_docs.sh --open
```

## What gets indexed

The `INPUT` block in `Doxyfile` covers:

- `README.md` (used as the rendered main page via
  `USE_MDFILE_AS_MAINPAGE`)
- `src/` (Ring 1 boot + Ring 5 secure-side substrate)
- `libs/` (every first-party library: `ra_core`, `ra_hal`,
  `ra_net_pal`, `ra_usb_pal`, `ra_nsc`, `ra_board_ek_ra8d2`,
  `ra_mpu`, etc.)
- `examples/` (every per-app demo under `examples/<tier>/<app>/`)
- `docs/reference/README.md` (links to the committed Renesas
  datasheet / hardware-user's-manual PDFs)

`libs/third_party/` is explicitly excluded -- vendored SOUP is
documented under `docs/SOUP/` instead, not via Doxygen.

## How to read the generated HTML

After `make docs`, navigate to `build/docs/html/index.html`. The
left-hand sidebar is grouped as:

- **Main Page** -- the rendered project README.
- **Files** -- one entry per `.c` / `.h` / `.cpp` / `.hpp` /
  `.md` file. Each file page surfaces its `@file` block, the list
  of declared symbols, and (if Graphviz is on PATH) include /
  caller graphs.
- **Globals** -- alphabetical index of every documented symbol.
- **Data Structures** -- every `struct`, `enum`, `union`, and
  `typedef`. `EXTRACT_ALL = YES` means even file-static helpers are
  surfaced; `EXTRACT_PRIVATE = NO` keeps NSC-private and
  `priv_*` helpers hidden by default.

Each function page renders the full Doxygen tag set the project
mandates (see `CLAUDE.md` "Doxygen Documentation Requirements"):
`@brief`, `@details`, `@param[in/out]`, `@return` / `@retval`,
`@pre`, `@post`, `@note`, `@warning`, `@par MC/DC:`, `@since`,
`@see`. Cross-references resolve to other pages automatically.

## Workflow

When you add a new function, struct, enum, or file:

1. Write the full Doxygen header per the `CLAUDE.md` rules (every
   applicable tag must be present -- this is gated by
   `make tidy`).
2. Run `make docs` locally and confirm the new symbol appears in
   the rendered HTML.
3. Tail `build/docs/doxygen-warnings.log` for any new warnings
   triggered by your change. The repository goal is **zero new
   warnings** (the existing log baseline is documented under
   `docs/MCDC_GAPS.md` along with the wider quality sweep).
4. Open `build/docs/html/index.html` in a browser and verify the
   page renders, links resolve, and any `@code ... @endcode`
   examples are syntax-highlighted.

## Configuration knobs

Common tweaks (edit `Doxyfile`):

- `EXTRACT_PRIVATE = NO` -- flip to `YES` to also document
  `priv_*` helpers (rarely useful).
- `HAVE_DOT = YES` -- requires Graphviz; the wrapper auto-disables
  this when `dot` is missing.
- `DOT_GRAPH_MAX_NODES = 75` -- bump to render larger call graphs;
  costs build time.
- `INPUT` -- add new top-level directories here when introducing
  a brand-new library.

## Common issues

- **"Found end of C comment inside a backtick block"** -- a
  Markdown code fence in a Doxygen comment was opened with `` ` ``
  but never closed before the comment terminator. Audit the
  offending file and balance the backticks.
- **"the name 'examples/<old>/foo.c' supplied as the argument in
  the \\file statement is not an input file"** -- the
  `@file <path>` in the file header does not match the file's
  on-disk location. Update the `@file` line.
- **"unable to resolve reference to ..."** -- you cited a symbol
  via `\see` or `\ref` but Doxygen could not find it. Either fix
  the spelling or document the missing target.

## Continuous integration

The `.github/workflows/` pipeline does not currently re-build the
Doxygen HTML on every PR (the warning-log baseline is large
enough that gating new commits would block too aggressively). The
recommended local workflow is to run `make docs` before pushing
any change that touches public API surface.
