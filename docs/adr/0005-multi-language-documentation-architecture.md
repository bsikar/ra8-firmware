# ADR-0005: Multi-language documentation architecture

* **Status:** Superseded on 2026-09-18 by `a67c04f3e`, "ci(docs): dismantle
  Doxygen, MkDocs, and HTML site generation infrastructure". Nothing this
  record decided is in the tree. That commit removed both generators that
  existed, their configuration, their theme, their provisioning and publishing
  builders, the prose-reachability checker, and the three generator scripts:
  24 files, -5948 lines; `git show a67c04f3e --stat` is the list. No site is
  published and no documentation generator runs. The record is kept for the
  reasoning, not as a description of the tree.
* **Date:** 2026-09-16

## Context

Doxygen has been the whole documentation story: it renders the published
site, indexes the manuals under `docs/`, parses the C sources and headers,
audits `@since` tags, and backs the doc-attachment and warning gates.

That worked while every first-party line was C. It no longer holds. Libraries
are moving to Zig behind unchanged C headers (`epic:zig-migration`), the
host and HIL harnesses are Rust, and the tooling is Python and Go. Doxygen
cannot parse Zig or Rust at all: it does not read `///` doc comments, struct
declarations, comptime constructs, traits, or safety contracts in either
language. So as `libs/ra8_box/src/ra8_box.c` became `root.zig` plus
`ra8_box_abi.zig`, the implementation documentation did not move to a
different page. It vanished, silently, with no gate failing.

The two obvious escapes are both wrong. Teaching Doxygen to lex Zig through
`EXTENSION_MAPPING` produces confident nonsense, because the mapping only
tells Doxygen to try C's grammar on a language that does not have it.
Hand-maintaining Markdown mirrors of Zig and Rust APIs produces documentation
that drifts from the source the first time someone is in a hurry.

## Decision (superseded)

Four generators, each scoped to what it can actually read, assembled into one
published site. None of the four was ever wired; `a67c04f3e` removed the two
that existed.

1. **Markdown hub: MkDocs with the Material theme.** Owns every prose page
   under `docs/`: architecture, ADRs, toolchain and CI manuals, qualification
   evidence, the SOUP inventory. Configured by a top-level MkDocs
   configuration, built by `just docs::hub` into `build/docs/hub/`. Both the
   configuration and the recipe were deleted in `a67c04f3e`.
2. **C ABI: Doxygen.** Scoped to the public headers (`libs/*/inc/*.h`) and the
   remaining C sources. It stays authoritative for the ABI, and it keeps the
   `@since` audit and the doc-attachment gate, because the C headers stay
   hand-authored no matter which language implements them. Mounted at
   `/api/c/`.
3. **Zig internals: Zig autodoc** (`zig build-lib -femit-docs`), per migrated
   library. Mounted at `/api/zig/<lib>/`.
4. **Rust: rustdoc** (`cargo doc`), over the host tools and HIL crates.
   Mounted at `/api/rust/`.

The boundary between (2) and (3) is the ABI membrane the migration already
draws. A caller reads the C ABI reference and sees the contract it always
had; someone working inside the library reads the Zig reference. Neither
generator is asked to describe the other's artifact.

### Why MkDocs Material for the hub

The repository already carries a Python toolchain: `pyproject.toml`, a
`uv.lock`, and roughly every check under `scripts/checks/` is Python. MkDocs
installs into that world with a pinned requirements file and no new language
runtime on a CI runner. mdBook would require a Rust toolchain in the docs job
purely to render Markdown; Starlight would require Node and a
`node_modules/`. Both were rejected on that cost alone, not on output
quality.

Two MkDocs features matter for the fail-closed posture the project already
holds its other gates to: `strict: true` turns a broken internal link or an
unrecognised config key into a build failure, and `exclude_docs` (MkDocs
1.6+) states in one place which files under `docs/` are not prose.

### Published layout

```
/            the hub
/api/c/      doxygen, public C ABI
/api/zig/    zig autodoc, per migrated library
/api/rust/   rustdoc
```

`just docs::build` remains the C ABI reference and `just docs::hub` the hub.
A later slice adds the assembler that stages all four into one tree, and the
single CI job that builds every generator and fails when any one of them
breaks.

### Generated output is never committed

Every generator writes under `build/`, which is git-ignored, and the site is
rebuilt from source on publish. This is the existing Doxygen policy (the
orphan `gh-pages` branch is force-updated with one fresh commit per run) and
it extends unchanged to the other three.

### No blind spots

The existing gates were to be preserved rather than replaced. Read the right
column as what this record intended, not as what runs: `a67c04f3e` took the
Doxygen warning gate, the `@since` audit, and the doc-attachment gate with it,
and the three Markdown, Zig and Rust columns were never wired at all.

| Gate | Today | Under this ADR |
| --- | --- | --- |
| Doxygen warnings | fail-closed over all of `docs/` and the C tree | fail-closed over the C ABI scope |
| `@since` audit | Doxygen tags | unchanged, C headers stay hand-authored |
| doc-attachment | C declarations | unchanged |
| Prose link integrity | not checked | `mkdocs build --strict` |
| Prose reachability | not checked | a nav-completeness checker (deleted unwired) |
| Zig doc comments | not checked | autodoc build, in the CI job slice |
| Rust doc warnings | not checked | `cargo doc` with warnings denied, same slice |

## Consequences

* A reader now has two entry points to know about, the hub and `/api/c/`,
  rather than one. The hub's landing page carries the map, and the assembler
  slice makes `/` the single published root.
* Four generators is four version pins, four provisioning paths, and four
  ways for CI to break. The pinned-tool cache the Doxygen build already uses
  is the model for the other three.
* The prose nav becomes a file someone has to update. That was deliberate: the
  nav-completeness checker was to fail when a Markdown file under `docs/` was
  neither in the nav nor explicitly excluded, so the cost of forgetting would
  be a red check rather than an orphaned page nobody ever finds. It was
  deleted unwired, so forgetting costs nothing today.
* Doxygen stops being the documentation system and becomes one generator
  among four. Its scope narrows to the C ABI in a later slice; nothing about
  its gates weakens in the process.

## References

* #900, this architecture
* #855 `epic:zig-migration`, RA8FW-339 build-graph parity
* `docs/DOCS.md`, the Doxygen pin and theme
* [MkDocs configuration](https://www.mkdocs.org/user-guide/configuration/)
