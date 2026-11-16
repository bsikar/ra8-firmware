# ra8-firmware documentation

Bare-metal firmware for the Renesas RA8 family (RA8D2 / RA8P1): a
hand-written HAL, RTOS / USB / networking ports, and a host emulator that
boots the real firmware ELFs.

This directory holds the in-tree firmware reference: architecture, decision
records, toolchain manuals, and the third-party inventory. CI gate manuals
live in `bsikar/ra8ci`, and the qualification document family lives in the
`RA8FW` YouTrack knowledge base under `RA8FW-A-2`. The API references are
generated from the source by the tool that can actually read each language.

## API references

| Mount point | Covers | Generator |
| --- | --- | --- |
| `/api/c/` | the public headers under `libs/*/inc/` | Doxygen |
| `/api/zig/<lib>/` | the migrated Zig library internals | `zig build-lib -femit-docs` |
| `/api/rust/` | the host tools and HIL crates | rustdoc |

A library that has moved to Zig keeps its hand-authored C header, so a caller
reading the C ABI reference sees the same contract it always had. The Zig
reference is for someone working *inside* that library. ADR-0005 records why
the split is drawn there and which generator owns which artifact, though it is
superseded: `a67c04f3e` removed the generators it decided on, so read it for
the reasoning rather than as a description of the tree.

!!! note "Mount points land with their generators"

    Those paths are the agreed published layout, not pages that exist yet.
    Each goes live in the slice that wires its generator into the build, and
    they are deliberately not links until then: this site fails its build on
    a dead link, and a promise dressed up as a hyperlink is exactly the thing
    that gate exists to catch.

## Where to start

- New to the tree: [Architecture overview](ARCHITECTURE.md), then
  [Modules](MODULES.md) and [Rings and worlds](RING_AND_WORLD.md).
- Setting up a machine: [Toolchain](TOOLCHAIN.md),
  [Python environments](PYTHON_ENVIRONMENTS.md), [IDE setup](IDE.md).
- Touching a board: [Hardware bring-up](HARDWARE_BRINGUP.md) and
  [Debugging](DEBUG.md).
- Changing anything that ships: [Style guide](STYLE_GUIDE.md),
  [Stack usage](STACK_USAGE.md), and [Zig ABI](ZIG_ABI.md).
- Why something is the way it is: the [decision records](adr/README.md).
- Qualification planning, verification, and accomplishment documents: `RA8FW-A-2`
  in the `RA8FW` YouTrack knowledge base.
- CI and host-build gate manuals: `docs/` in `bsikar/ra8ci`.

## Documentation checks and recipes

There is no generated HTML site. See [`DOCS.md`](DOCS.md) and `just/docs.just`
for the live documentation layout, the `just docs::*` reporting recipes, and
the documentation gates maintained in `bsikar/ra8ci`.
