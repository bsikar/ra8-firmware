# KiCad library standards

## Project structure

Each board is an independent KiCad project. Components are shared by function,
not by the board that first used them. Do not install these parts into global
user libraries.

```
ra8p1_kicad/
  libs/
    symbols/<Category>.kicad_sym
    footprints/<Category>.pretty/<Footprint>.kicad_mod
    3dmodels/<Category>.3dshapes/<Model>.step
  ereader/
    ereader_rev1.kicad_pro
    ereader_rev1.kicad_sch
    ereader_rev1.kicad_pcb
    sym-lib-table
    fp-lib-table
  resources/                 Manufacturer references and source packages
  design/                    Electrical requirements and design rationale
```

Both `.step` and `.stp` model suffixes are accepted. Preserve manufacturer
filenames, especially where a model is shared between device families.

The categories are `Processors`, `Wireless`, `Memory`, `Power_Devices`,
`Connectors`, `Protection`, `Timing`, and `Sensors`. Add a category only when
the first actual part requires it. A future Gaggia project belongs in its own
board directory and can register these same libraries; do not duplicate parts.

## Portable references

Register symbols and footprints in each board's project-local tables:

```
Symbol library: ${KIPRJMOD}/../libs/symbols/Processors.kicad_sym
Footprint library: ${KIPRJMOD}/../libs/footprints/Processors.pretty
Model: ${KIPRJMOD}/../libs/3dmodels/Processors.3dshapes/<filename>.stp
```

Symbol footprint properties use `Category:Footprint`, for example
`Processors:BGA289C65P17X17_1200X1200X138`. Every reference must resolve from
the board directory. Do not retain old library aliases after moving parts.

## Schematic appearance

The visual style follows the relevant [KiCad library conventions](https://klc.kicad.org/):

- Pin names, pin numbers, reference and value text: 50 mil (1.27 mm).
- Body stroke: 10 mil (0.254 mm), solid, with body-background fill for ICs.
- Pin length: 150 mil (3.81 mm) by default. Use 200 mil (5.08 mm) consistently
  throughout a symbol when its imported pin numbers exceed three characters.
- Connection endpoints on a 100 mil (2.54 mm) grid; rows spaced at 100 mil.
- Pin-name offset: 20 mil (0.508 mm).
- Reference above value, centered above the body; hidden metadata at the origin.
- Body width accommodates the longest opposing pin names plus a clear gap.
  Body height follows its pin rows. Consistent style does not mean identical
  rectangle dimensions regardless of function.
- IC units use a shared top datum so fields align between units of different
  heights. This is a project-specific placement convention, not a claim of
  complete upstream KLC compliance.
- Crystals retain conventional resonator graphics; never replace them with a
  generic IC rectangle merely to make all parts look identical.

Split large ICs into non-interchangeable functional units when that improves
readability. Use normal reference suffixes (`U1A`, `U1B`, etc.), an internal
functional heading, and exactly one occurrence of each package pin across all
units. Do not duplicate supply pins in every unit. Units remain one physical
component with one footprint and one BOM entry.

Use `U` for ICs, `J` for connectors, `D` for protection arrays, and `Y` for
crystals. Preserve part numbers and manufacturer descriptions. Before electrical
acceptance, Datasheet fields must identify manufacturer documents or valid
project-relative references. Inherited product-page and distributor links are
preserved by the appearance cleanup and remain unverified source metadata.

## Electrical and mechanical acceptance

Use standard KiCad `Device` primitives for ordinary passives and `power`
symbols for global supplies and ground. Register the bundled libraries with
`${KICAD10_SYMBOL_DIR}` in the project table; do not modify the user's global
libraries or create substitute resistor/capacitor/inductor graphics. Cached
symbols in each schematic preserve its rendering when opened elsewhere.

Draw decoupling banks with visible common rail and return wires, junctions at
actual branches, and named pin-pair placement notes. Power arrows point up,
grounds point down, and signal paths normally flow left to right. Use local
labels for sheet-local signals and hierarchical pins for inter-sheet signals;
avoid replacing visible short connections with repeated labels.

`PWR_FLAG` is an ERC source declaration, not a supply name or a substitute for
a regulator. Place it only at a justified source or after a passive element
that separates the source's power-output pin from the powered net. State the
source beside any non-obvious flag. Do not flag an unimplemented supply just
to suppress an undriven-power error. Never hide unfinished wiring with
no-connect markers or globally weaken ERC rules.

Appearance cleanup is not electrical qualification. Untouched imported units
retain unverified names and passive/unspecified electrical types, including
unusual ESP32 ground sub-pad numbering. The selected 289-ball RA8P1 core,
DCDC, I/O-supply, and analog units have explicit power-pin names and types
checked against its pin list; this does not qualify the remaining units or
their footprints. Resolve the shared VLO output model before final ERC
acceptance. Check each remaining symbol against its exact ordering-code
datasheet before wiring, and qualify footprints before PCB work. Do not infer
safety from an ERC result on all-passive imported symbols.

The 289-ball USB/MIPI unit now uses normalized manufacturer pin names,
power-input types for its nine supply/ground pins, and bidirectional types
for the four USB data pins. USBHS_RREF remains passive for its external
reference resistor. The six unused MIPI lanes still retain passive types;
their explicit no-connect treatment is not qualification for active MIPI use.
Authority: RA8P1 Datasheet Rev.1.30 Table 1.17; RA8x2 Quick Design Guide
Rev.1.10 Tables 1-2; RA8P1 HUM Rev.1.30 section 21.4 for unused MIPI.

New design net labels use `COPI`, `CIPO` and `CS` instead of legacy SPI terms.
Review imported pin-name aliases against manufacturer documentation before
renaming them; visual normalization must not silently change their identity.

Footprints and 3D models describe physical dimensions, not schematic styling.
Preserve pad numbers, pad sizes, pitch, mask/paste settings, courtyard,
silkscreen, keepouts and model transforms during a library move. Validate
those separately against manufacturer drawings before PCB layout.

## Change checks

1. Preserve a recoverable copy before bulk conversion.
2. Compare complete pin inventories before and after, including pin number,
   name, electrical type, graphic type, visibility and alternate functions.
3. Confirm all symbol-to-footprint and footprint-to-model paths resolve.
4. Compare footprint geometry and model bytes against the source.
5. Export every unit with KiCad and inspect the rendered graphics.
6. Update schematic library IDs and cached symbols together. Existing wired
   sheets require a connectivity-preserving migration, not blind replacement.
7. Remove superseded libraries only after the replacements and references pass.

Version the hardware project, local libraries, references, and complete
schematic PDF together. Exclude machine-local KiCad state and editor history.
Use Git LFS for reference archives exceeding GitHub's regular file-size limit.
Keep project and library files directly editable after a normal clone.
