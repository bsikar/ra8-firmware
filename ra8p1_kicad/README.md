# RA8P1 e-reader electronics

Open `ereader/ereader_rev1.kicad_pro` in KiCad 10.0.5 or a compatible newer
version. The project includes its schematic, board, project configuration,
project-local library tables, symbols, footprints, and STEP models. Library
paths resolve relative to the project; standard `Device` and `power` symbols
use the bundled KiCad 10 libraries through `${KICAD10_SYMBOL_DIR}`. No custom
global library installation is needed.

The schematic has a root index and separate processor interface, core-power,
I/O-supply, and clock/reset/debug sheets. The power circuits are drafts with passive qualification
still open; the processor interfaces and remaining subsystems are unfinished.
The PCB is empty.
Neither is a manufacturing release. Imported component models still require
electrical and package qualification before use in a finished design.

## Clone and open

Install Git LFS before cloning, or run `git lfs pull` after cloning. The large
manufacturer reference-design ZIP uses LFS. The editable KiCad project and
component libraries are ordinary Git files. Manufacturer reference files retain
their original notices and are reference material, not this board's design.

Open the project from its checked-out location. Do not add absolute model paths
or depend on a user's global symbol/footprint tables. KiCad lock files, personal
view settings, and editor history are excluded from commits.

## Review and export

`exports/ereader_rev1.pdf` contains every schematic sheet and is refreshed for
each design commit. It represents the design at that commit, including any
explicitly incomplete sections. It is not a fabrication drawing of the PCB.

From the repository root, regenerate it with:

```sh
./ra8p1_kicad/scripts/export_design.sh
```

The script also works when called by absolute path from another directory.
It finds `kicad-cli` on PATH or in the standard macOS KiCad app bundle. Set
`KICAD_CLI` to select another executable. An optional schematic path exports
that project's complete hierarchy to `exports/<schematic-name>.pdf`.

The script exports saved files, including all child sheets, and replaces the
previous PDF only after KiCad succeeds and produces a nonempty PDF. It does
not save unsaved editor changes. `--help` shows the available arguments.

Before each commit, save all sheets, export the complete PDF, inspect every
page, and run ERC. During circuit development, record unresolved findings;
do not hide unconnected pins merely to obtain a clean report. Before pushing,
run the repository checks required by `../CLAUDE.md`.

## Design references

- [Hardware requirements](design/ereader_requirements.md)
- [Library conventions](LIBRARY_STANDARDS.md)
- [Imported parts inventory](PARTS-CHECKLIST.md)
- [Hardware epic and section issues](https://github.com/bsikar/ra8-firmware/issues/821)

The Gaggia controller is a separate future board. Shared components belong in
the functional libraries under `libs/`; board-specific sheets belong in their
own project directory.
