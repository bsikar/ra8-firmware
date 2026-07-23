# Binary Format Specifications -- Overview

This section specifies every **first-party binary format** this firmware
produces or consumes. It exists because the code alone is a poor teacher: you
can read `ra8_jof_parse()` and learn *what* the bytes are without ever
learning *why* they are arranged that way, and the "why" is where all the
device constraints live.

Each specification is written for a **competent engineer who is new to that
particular format**. Every page follows the same progression, so you can stop
at whatever depth you need:

| Section | What it answers | Stop here if... |
|---------|-----------------|-----------------|
| 1. Synopsis | What is it, what problem does it solve? | you just need to hold a conversation about it |
| 2. Design rationale | Why this and not an off-the-shelf format? | you are reviewing a design decision |
| 3. Wire format | Exact bytes: offsets, widths, ranges | you are writing a parser |
| 4. Algorithms | How producing and consuming actually work | you are changing the producer or reader |
| 5. Memory behaviour | What is resident, what streams, what it costs | you are budgeting SRAM |
| 6. Worked example | Real bytes from a real file, annotated | you are debugging a specific file |
| 7. Failure modes | What a malformed or hostile file can do | you are threat-modelling |
| 8. Versioning | How revisions are detected and rejected | you are adding a field |

---

## The inventory

Confirmed by sweeping the tree for four-byte magic constants. The table
separates **content formats** (a file or blob that carries data across a
trust or time boundary, and therefore earns a full specification) from
**internal markers** (a tag that identifies an in-memory or build-time
artifact, and only earns an entry here).

### Content formats -- full specifications

| Magic | Format | Home | Producer | Specification |
|-------|--------|------|----------|---------------|
| `JOF1` / `JOFE` | Jump-Offset band-tile atlas | `libs/ra8_jof` | `ra8_jof_produce()`, `ra8_fmt convert` | @ref md_docs_2formats_2JOF |
| `RBKC` | Chunked `.rabook` container | `libs/ra8_book` | `tools/epub_compile` | @ref md_docs_2formats_2RBKC |
| `NPU1` | `.npub` Ethos-U55 model container | `libs/ra8_hal` | `tools/vela/vela_gen.py` | @ref md_docs_2formats_2NPU1 |
| `ROT1` | Root-of-trust signed-image trailer | `libs/ra8_dfu` | `tools/rot_sign.py` | @ref md_docs_2formats_2ROT1 |
| `NSR1` | Non-Secure image RoT header | `libs/ra8_tz_secure_boot` | `sign_and_merge.py`, `ns_image.ld` | @ref md_docs_2formats_2NSR1 |

Each has its own page in this section; they are also listed in the navigation
sidebar under "Binary format specifications".

### Suggested reading order

They are not independent, and reading them in dependency order costs less
effort than the alphabetical sidebar suggests:

1. @ref md_docs_2formats_2JOF -- the fullest treatment, and the one that
   establishes the vocabulary (index, seek, working set, edge clamp) the others
   reuse.
2. @ref md_docs_2formats_2RBKC -- the same seekability problem for a flat blob;
   introduces the two-layer container idea and the front-loaded table.
3. @ref md_docs_2formats_2ROT1 -- the shift from "do not crash on bad input" to
   "do not run unauthorised code".
4. @ref md_docs_2formats_2NSR1 -- eight bytes that make ROT1 usable at the
   TrustZone boundary.
5. @ref md_docs_2formats_2NPU1 -- the same host-does-the-work philosophy applied
   to a neural network.

### Internal markers -- no separate specification

These are deliberately **excluded** from the full treatment. Each is a tag on
an artifact that never crosses a trust boundary as a standalone file, so there
is nothing to parse defensively and no wire contract to pin down. Documenting
them as "formats" would imply a stability guarantee none of them has.

| Magic | What it actually is | Why no spec |
|-------|--------------------|-------------|
| `RBK1` | A four-byte stamp (`k_ra8_rabook_import_stamp_magic`, `0x52424B31`) the importer writes to mark a blob as having passed import | Not a container. It is one word inside a structure the same library owns end to end -- there is no independent reader, so there is no wire contract. Covered where it is used, in `ra8_rabook_import.h`. |
| `NPUQ` | A four-character tag on the NPU quantisation helper (`s_tag` in `ra8_npu_quant.c`) | A logging/identification string in one translation unit. It is never serialised to a file. |
| `SE55` | "Sim-Ethos-U55" marker in the high bits of a simulated NPU command word (`ra8_npu_sim_cmd.h`) | A *register-level convention* between the firmware and `board_sim`, not an on-disk format. It exists so a host test can distinguish a simulated command from noise. Specified where it belongs, in `ra8_npu_sim_cmd.h`. |

Two more four-character strings turn up in a naive grep and are **not** magics
at all, recorded here so the next person does not re-investigate them:
`RAIO` is a FAT volume label used by the `ra8_io` demo apps, and `BLEN` is the
backlight-enable **signal name** in the EK-RA8D2 board pin table.

---

## Why not an off-the-shelf format?

Every page in this section has to answer the same challenge, and it is the
first thing a reviewer asks: *a standard format already does this -- why is
there a bespoke one instead?* The challenge is fair, and for each format there
is a specific competitor a knowledgeable reader will name.

| Format | The off-the-shelf competitor | Does it already solve seeking? | What actually decided it |
|---|---|---|---|
| `JOF1` | **Tiled TIFF** -- `TileOffsets` / `TileByteCounts`, `COMPRESSION_ADOBE_DEFLATE` | **Yes.** That pair genuinely is a jump-offset index over independently-compressed tiles | Parser attack surface, and a producer that must never seek |
| `RBKC` | **Blocked DEFLATE** -- `bgzf`, `dictzip`, or `.xz` with its block index | **Yes.** Independent blocks plus an index is exactly this pattern | One chunk == one `ra8_vmem` cache frame == one inflate |
| `RABOOK1` | **EPUB, read directly** | Not the question -- the cost here is parsing, not seeking | Pre-resolving the CSS cascade and pre-transcoding images to panel-native 4 bpp |

Two of those three competitors **already solve random access.** That is worth
saying plainly, because "the standard format cannot seek" is the argument these
pages are most likely to be *assumed* to make, and for two of them it would
be false.

### The thread that actually connects them

What every one of these formats does is move a **variable, input-dependent
cost** off the device permanently, and pay it once on a host with an operating
system and gigabytes of RAM.

- `JOF1` moves the transcode that makes an image randomly accessible.
- `RABOOK1` moves the unzip, the XML parse and the CSS cascade.
- `RBKC` moves the decision of where the compression boundaries fall.

The fixed-offset wire layouts they share are a **consequence, not the goal.**
Once the device is no longer parsing content, the only thing left on it to
parse is a table of offsets -- and a table can be fixed-width, bounded, and
validated with arithmetic. The bounded parser falls out of moving the work,
which is why the same shape appears four times.

The binding constraint behind all of it, stated once: a bare-metal reader with
**zero dynamic allocation** (NASA P10 Rule 3) and a **1.6 MB** SRAM budget,
facing a file that arrived on a removable card and is therefore untrusted. A
format that must allocate in order to be read is not a candidate, however well
specified it is.

### What this costs, honestly

A rationale that only lists wins is not trustworthy. The bill for owning four
bespoke formats is real and is paid continuously:

- **No off-the-shelf tooling -- so we built our own, twice.** A tiled TIFF
  opens in any image viewer and a CBZ in any comic reader; a `.jof` opens in
  nothing that already exists. This tree therefore carries *two* first-party
  tools to recover what a standard format gets for free: `tools/ra8_fmt` to
  inspect the bytes, and `tools/ra8_viewer` to actually look at a page.
  Diagnosing a real rendering bug meant building the inspector first, before a
  single byte could be read -- a cost a standard format charges at zero.
- **A specification per format.** These pages exist only because the formats
  are novel. Nobody writes a 900-line document explaining how to read a TIFF.
- **No independent validation.** There is no `tiffinfo`, no fuzzing corpus
  accumulated over thirty years, and no second implementation to disagree with
  ours and expose a bug. `ra8_fmt inspect` is the only checker, written by the
  same hands as the writer it checks.
- **Ownership with no upstream.** Every format here is a permanent maintenance
  obligation. A bug is ours; a missing feature is ours to add.

The trade is accepted because the alternative is worse **on this specific
device**, not because the standard formats are bad. On a machine with a heap
and an MMU, most of these decisions would go the other way.

### Where the parallel breaks

The three cases are not equally strong, and flattening them into one story
would misrepresent one of them.

- **JOF vs tiled TIFF is the closest contest** and the only one where parser
  attack surface is the deciding factor. It gets the detailed treatment, in
  @ref md_docs_2formats_2JOF section 2.
- **RABOOK's justification is the weakest of the three, and is partly
  historical.** The device parses EPUB directly today: `libs/ra8_epub` opens
  the ZIP with miniz and parses the OPF with tinyxml2, and ten firmware
  applications link it, several of them silicon-validated. "The device cannot
  read an EPUB" is therefore not true, and has not been for some time. The
  defensible part is narrower -- pre-resolving the CSS cascade and
  pre-transcoding images to 4 bpp is real work genuinely moved off the device.
  But the compiled path does **not** eliminate runtime parsing the way its own
  documentation implies: `ra8_book_chapter_to_xhtml()` serialises the
  pre-parsed DOM *back into XHTML* so `ra8_reflow_layout_chapter()` can parse
  it again. Anyone extending `ra8_book` should know that before relying on
  "never parses XHTML at runtime" as a property.

---

## Conventions shared by every format

### Endianness

**Every multi-byte integer in every first-party format is little-endian.** The
Cortex-M85 runs little-endian, so this is a zero-cost choice on device and the
producers (Python and C host tools) match it explicitly. There is no
byte-swapping anywhere in the read paths.

### Two kinds of magic -- and why the bytes look reversed

This trips people up in a hexdump, so it is worth being explicit. The formats
split into two camps:

| Style | Formats | Declared as | Bytes on disk |
|-------|---------|-------------|---------------|
| **Byte string** | `JOF1`, `JOFE`, `RBKC` | a 4-byte array, compared with `memcmp` | read left-to-right: `4a 4f 46 31` = `JOF1` |
| **`uint32` constant** | `NPU1`, `NSR1`, `ROT1`, `RBK1` | a `uint32_t` enum, compared with `==` | stored little-endian, so whether they read forwards depends on how the constant was chosen -- see below |

A `uint32` magic reads forwards in a hexdump only if whoever picked the constant
wrote it "backwards" on purpose. Half of ours did and half did not:

| Magic | Constant | Little-endian bytes | ASCII column | Reads |
|-------|----------|--------------------|--------------|-------|
| `NPU1` | `0x3155504E` | `4e 50 55 31` | `NPU1` | **forwards** |
| `NSR1` | `0x3152534E` | `4e 53 52 31` | `NSR1` | **forwards** |
| `ROT1` | `0x524F5431` | `31 54 4f 52` | `1TOR` | **reversed** |
| `RBK1` | `0x52424B31` | `31 4b 42 52` | `1KBR` | **reversed** |

`NPU1` and `NSR1` were deliberately spelled so the flashed image reads its own
marker in order -- convenient when you are staring at a hexdump of an NS image
or a `.npub` blob. `ROT1` and `RBK1` were written the "natural" way, so they
appear reversed on disk. Neither is a bug; both compare identically with `==`.
Just do not assume a `uint32` magic reads forwards, and do not "fix" `1TOR`
when you see it.

### The fourth byte is a version discriminator

Every magic ends in a character that identifies the **revision or role** of the
structure, not just the family:

```
  J O F 1          R B K C          J O F E
  \___/ |          \___/ |          \___/ |
    |   |            |   |            |   |
  family|          family|          family|
        |                |                |
   version 1        "Chunked"        "End" (footer)
```

This makes rejection cheap and unambiguous. A reader compares four bytes; if
they do not match exactly, it refuses the file. It never tries to parse a
structure it does not fully recognise, so an unknown future revision produces a
clean "I do not understand this file" rather than a plausible-looking
misparse. Adding an incompatible revision means spending a new fourth byte
(`JOF2`), and every existing reader rejects it automatically with no code
change.

Two formats additionally carry an explicit `version` **field** alongside the
magic (`NPU1`, `ROT1`). Those use the magic for family identification and the
field for revision, which is the better design when the format expects to
revise often -- but the discriminator byte is still checked first.

### Validation posture: fail-closed

Content formats arrive from untrusted media -- an SD card the user filled, an
EPUB downloaded from anywhere. Every reader in this tree is **fail-closed**:
a field is validated before it is used, never after, and any failed check
aborts the whole operation with a `ra8_err_t` rather than clamping and
continuing.

The threat being defended against is **not** remote code execution from an SD
card. It is the far more likely failure that actually matters in practice:
*the application or an EPUB crashing because a file was malformed or hostile*.
So the bar every reader must clear is that no input, however corrupt, causes an
out-of-bounds access, an unbounded loop, an unbounded allocation, or a hang.

Three mechanisms do most of that work:

1. **Bounds are derived, then checked against the container size.** A reader
   never trusts an offset because it was in the file; it re-derives the legal
   window and confirms the offset lies inside it.
2. **Loops are bounded by a validated count** (NASA Power of 10 Rule 2), and
   the count itself is range-checked before the loop is entered.
3. **Decompression is capped** by `ra8_decomp_limits_t` -- a hard **64 MiB**
   output ceiling and a **1024:1** expansion-ratio ceiling -- so a
   decompression bomb fails instead of exhausting memory. This applies to every
   DEFLATE/zlib stream in `RBKC` and `JOF`.

### Zero dynamic allocation

NASA Power of 10 Rule 3 holds throughout: no reader in this section allocates.
The caller supplies every buffer at open time (offset tables, metadata tables,
staging, scratch, output pixels). This is why the readers take so many buffer
arguments -- it is a deliberate contract, not an ergonomic oversight, and it is
what makes the working set statically knowable.

---

## The `ra8_fmt` tool

Every worked example in this section was produced with `tools/ra8_fmt`, the
in-tree inspector. Build it:

```
cmake -S tools/ra8_fmt -B build/ra8_fmt
cmake --build build/ra8_fmt
```

It exposes three verbs over a format registry:

```
  ra8_fmt convert --format <fmt> --in <file> --out <file>
  ra8_fmt inspect <container> [--verbose]
  ra8_fmt verify  --format <fmt> --in <file> [--out <dump.ppm>]
```

The three verbs answer three different questions, and the distinction matters:

| Verb | Input | Question it answers |
|------|-------|---------------------|
| `convert` | a **source** file (PNG/JPEG) | produce the first-party container |
| `inspect` | a **container** | is this structurally sound, and what is in it? `--verbose` adds header/footer hexdumps and a per-record table |
| `verify` | a **source** file | does the transcode round-trip losslessly, byte for byte? |

`inspect` sniffs the magic itself, so `ra8_fmt inspect foo.bin` identifies the
container without being told which format it is; for `JOF` it also proves the
tile grid covers the image exactly once, with no gaps and no duplicates. Note
that `verify` takes the **source**, not the container -- it re-runs the
transcode and diffs the result against a reference decode, which is how the
no-quality-loss rule is checked mechanically rather than asserted.

Using the real tool rather than hand-written examples is a deliberate policy:
invented hexdumps drift silently from the code, and a reader who cannot
reproduce the example cannot trust it. Every byte quoted in these pages was
generated by running these commands.

---

## How the diagrams in this section are built

Every diagram on these pages is a Graphviz `@dot` block written inline in the
Markdown source. That is a deliberate choice, and worth recording so the next
person extending this section does the same thing:

- **The source is text, so it reviews and diffs.** A committed `.png` or `.svg`
  is opaque in a pull request and rots silently when the format changes. A dot
  block sits three lines below the prose it illustrates, and changing a field
  name in the diagram shows up in the diff exactly like changing it in a table.
- **It renders today.** `HAVE_DOT`, `DOT_IMAGE_FORMAT = svg` and
  `INTERACTIVE_SVG` are set in the `Doxyfile`, graphviz is provisioned on the
  documentation runners, and the `docs` gate hard-requires `dot`, so a missing
  graphviz fails loudly instead of quietly dropping every diagram.
- **`IMAGE_PATH` is empty on purpose.** Nothing here depends on a committed
  binary image asset. If you ever add one, set `IMAGE_PATH` in the `Doxyfile` in
  the same commit -- otherwise `@image` silently finds nothing.

**Do not use `@startuml` anywhere in the tree.** `PLANTUML_JAR_PATH` is not
configured and no JVM is provisioned, so doxygen ignores every `@startuml`
block -- they render nowhere. Use `@dot`, which does.
`scripts/checks/check_doc_diagrams.py` enforces both halves: it rejects
`@startuml` outright, and it fails when the number of authored `@dot` blocks
does not match the number of diagrams that actually reached the generated HTML.

---

## Where the authority lives

For each format the **wire contract is owned by this specification section**;
the header keeps the enum of byte offsets, which is the machine-checkable
truth the compiler enforces. Where a header still carries a summary field
table, it is a convenience copy and says so, pointing here for the full
contract. When you change a format:

1. Change the offset/limit enums in the header (the code's source of truth).
2. Update the specification page here (the human contract, including rationale).
3. Regenerate the worked example with `ra8_fmt` so the hexdump matches.

@dot
digraph fmt_layers {
  bgcolor="transparent";
  rankdir=LR;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
        fillcolor="#e8eef7", color="#5a7ca6"];
  edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];

  src   [label="Source content\n(EPUB, CBZ, PNG,\nJPEG, .tflite, .elf)"];
  host  [label="Host producer\n(tools/epub_compile,\nra8_fmt, vela_gen,\nrot_sign)", fillcolor="#dff0e4", color="#5f9e72"];
  fmt   [label="First-party\nbinary format\n(JOF/RBKC/NPU1/\nROT1/NSR1)", fillcolor="#fbf0d9", color="#b8913f"];
  dev   [label="On-device reader\n(bounded RAM,\nzero alloc,\nfail-closed)", fillcolor="#f7e4e4", color="#b06a6a"];

  src -> host [label="parse once,\noff device"];
  host -> fmt [label="transcode"];
  fmt -> dev  [label="SD / MRAM /\nOSPI / ZIP"];
}
@enddot

The shape above is the reason this section exists at all: **all the expensive,
unbounded, untrusted parsing happens on the host**, once, and the device only
ever reads a format designed for it -- seekable, bounded, and checkable. Every
page in this section is a variation on that one idea.
