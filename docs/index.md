# ra8-firmware documentation

Bare-metal firmware for the Renesas RA8 family (RA8D2 / RA8P1): a
hand-written HAL, RTOS / USB / networking ports, and a host emulator that
boots the real firmware ELFs.

This site is the hub. It holds the prose: architecture, decision records,
toolchain and CI manuals, qualification evidence, and the third-party
inventory. The API references are generated from the source by the tool that
can actually read each language, and are mounted beside it.

## API references

| Mount point | Covers | Generator |
| --- | --- | --- |
| `/api/c/` | the public headers under `libs/*/inc/` | Doxygen |
| `/api/zig/<lib>/` | the migrated Zig library internals | `zig build-lib -femit-docs` |
| `/api/rust/` | the host tools and HIL crates | rustdoc |

A library that has moved to Zig keeps its hand-authored C header, so a caller
reading the C ABI reference sees the same contract it always had. The Zig
reference is for someone working *inside* that library. ADR-0005 records why
the split is drawn there and which generator owns which artifact.

!!! note "Mount points land with their generators"

    Those paths are the agreed published layout, not pages that exist yet.
    Each goes live in the slice that wires its generator into the build, and
    they are deliberately not links until then: this site fails its build on
    a dead link, and a promise dressed up as a hyperlink is exactly the thing
    that gate exists to catch. Track the remaining slices on
    [#900](https://github.com/bsikar/ra8-firmware/issues/900).

## Where to start

- New to the tree: [Architecture overview](ARCHITECTURE.md), then
  [Modules](MODULES.md) and [Rings and worlds](RING_AND_WORLD.md).
- Setting up a machine: [Toolchain](TOOLCHAIN.md),
  [Python environments](PYTHON_ENVIRONMENTS.md), [IDE setup](IDE.md).
- Touching a board: [Hardware bring-up](HARDWARE_BRINGUP.md) and the
  [HIL developer workflow](HIL_DEVELOPER_WORKFLOW.md).
- Changing anything that ships: [Style guide](STYLE_GUIDE.md),
  [MISRA](MISRA.md), [MC/DC](MCDC.md), [Coverage](COVERAGE.md).
- Why something is the way it is: the [decision records](adr/README.md).

## Building this site

```sh
just docs::hub          # build the hub into build/docs/hub/
just docs::hub_serve    # build and serve it locally with live reload
just docs::hub_check    # nav coverage gate, no generator required
just docs::build        # the C ABI reference (Doxygen)
```

Generated HTML is never committed. Everything lands under `build/`, which is
git-ignored, and the published site is rebuilt from source on every push to
`main`.

The hub runs `mkdocs build --strict`, so a link to a page that moved is a
build failure rather than a 404 a reader finds later. `just docs::hub_check`
is the cheaper half of the same promise: it fails when a Markdown file under
`docs/` is neither in the nav nor explicitly excluded, so a new manual cannot
land invisible.
