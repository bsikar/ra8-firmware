# NPU1 -- The `.npub` Ethos-U55 Model Container

**Magic:** `NPU1` (`0x3155504E`) &nbsp;|&nbsp;
**Library:** `libs/ra8_hal` &nbsp;|&nbsp;
**Extension:** `.npub` &nbsp;|&nbsp;
**Producer:** `tools/vela/vela_gen.py` &nbsp;|&nbsp;
**Applies to:** RA8P1 (RA8D2 + Ethos-U55 NPU)

---

## 1. Synopsis

NPU1 is how a trained neural network gets onto the RA8P1's Ethos-U55 NPU in a
form the firmware can execute with **zero parsing and zero allocation**.

The Ethos-U55 does not consume a `.tflite` model. It executes a **command
stream** -- a pre-compiled sequence of hardware operations -- produced by Arm's
Vela compiler, which takes a quantised TensorFlow Lite model and schedules it
against a specific accelerator configuration (MAC count, SRAM budget). Vela also
decides the *memory layout*: which arenas exist, how big each is, and which are
read-only weights versus runtime scratch.

The output of that compilation is several loosely-related artifacts: a command
stream, a weight blob, and a description of the tensor arenas the hardware
expects at `BASEP0..BASEPn`. NPU1 is the container that packs all of it into
**one flat, memory-mappable file** so the device does not have to reassemble it.

The design goal is the same one that runs through every format here, applied to
a new domain: **do the hard work on the host, exactly once.** By the time a
`.npub` reaches the device, every scheduling decision is baked, every offset is
precomputed, and loading a model means walking a fixed-width descriptor table
and handing pointers to the hardware. There is no model parsing on device, no
graph traversal, and no allocator.

The one runtime decision the loader makes is per region: a **BAKED** region's
bytes live inside the blob (so the pointer is `blob + data_offset`, in place,
never copied), and a **RUNTIME** region is carved out of a caller-supplied arena
in NPU-visible SRAM. Weights are baked; scratch and activations are runtime.

If you stop reading here: *NPU1 is a flat container holding a Vela command
stream, a region-descriptor table describing the Ethos-U55 tensor arenas, and
the baked weight data -- so loading a model is a table walk and a few pointer
assignments rather than a parse.*

---

## 2. Design rationale

### The comparison that makes it click

Compare running a model from a `.tflite` against running one from a `.npub`:

@dot
digraph tflite_vs_npub {
  bgcolor="transparent";
  rankdir=TB;
  fontname="Helvetica";
  node [shape=box, fontname="Helvetica", fontsize=10, style="filled,rounded"];
  edge [fontname="Helvetica", fontsize=9];

  subgraph cluster_tfl {
    label="Interpreting a .tflite on device: the model is a program you must compile at runtime";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b06a6a"; style="rounded"; bgcolor="#fbeeee";
    t0 [label="parse FlatBuffer\n(offset-chasing over untrusted data)", fillcolor="#e8c9c9", color="#b06a6a"];
    t1 [label="walk the operator graph,\nresolve tensors + shapes", fillcolor="#e8c9c9", color="#b06a6a"];
    t2 [label="plan arenas, allocate\n-> dynamic memory (breaks P10 Rule 3)", fillcolor="#e8a8a8", color="#8c3a3a", width=4];
    t3 [label="schedule ops onto the NPU\nevery single boot", fillcolor="#e8c9c9", color="#b06a6a"];
    t4 [label="inference", fillcolor="#d6efd9", color="#5f9e72"];
    t0 -> t1 -> t2 -> t3 -> t4;
  }

  subgraph cluster_npub {
    label="NPU1: Vela already compiled it -- the device only wires up pointers";
    fontsize=11; fontname="Helvetica-Bold";
    color="#5f9e72"; style="rounded"; bgcolor="#eef7f0";
    n0 [label="check magic + version + checksum", fillcolor="#dff0e4", color="#5f9e72"];
    n1 [label="walk region_count fixed-width\ndescriptors (16 bytes each)", fillcolor="#dff0e4", color="#5f9e72"];
    n2 [label="BAKED  -> pointer into the blob\nRUNTIME -> offset into caller arena\n(no allocation)", fillcolor="#cfe8d6", color="#5f9e72", width=4];
    n3 [label="point the NPU at the\ncommand stream", fillcolor="#dff0e4", color="#5f9e72"];
    n4 [label="inference", fillcolor="#d6efd9", color="#5f9e72"];
    n0 -> n1 -> n2 -> n3 -> n4;
  }
}
@enddot

The left column is a compiler running on a microcontroller, every boot, over
attacker-reachable data. The right column is a table walk. That is the entire
argument -- and it is the same "move the unbounded work to the host" move that
JOF makes for images and RBKC makes for books, which is why all four formats end
up looking structurally similar.

### Why baked and runtime regions are distinguished

A neural network's memory divides cleanly in two, and conflating them wastes the
scarcer resource:

- **Weights** are read-only, often the largest part of the model (megabytes),
  and identical on every inference. Copying them into RAM at load would burn the
  RAM budget for no benefit. Baked regions are used **in place** -- the loader
  hands the hardware `blob + data_offset`, so weights can execute directly out
  of XIP OSPI flash or MRAM and never occupy SRAM at all.
- **Scratch and activations** are written every inference and must live in
  NPU-visible SRAM. They have no meaningful initial contents, so storing them in
  the file would be storing zeros.

Hence one bit per region -- `k_ra8_npu_blob_rflag_baked` -- decides whether
`data_offset` is meaningful. It is a small flag carrying a large architectural
consequence: the file contains exactly the bytes that must persist and not one
byte more.

### Why the region table is indexed, not named

Descriptors carry a `role` (weights / scratch / input / output / other), but the
**command stream references regions by their index in the table** -- their
`BASEPn` slot -- not by role. The loader deliberately does not act on `role`
beyond reporting it.

That is the right split. Vela already decided the layout; if the loader tried to
interpret roles and reorder anything, it would be second-guessing the compiler
that produced the command stream, and any disagreement would be a silent
mismatch between what the stream addresses and what the loader bound. Keeping
`role` **informational** means there is exactly one authority on layout. The
field exists for diagnostics and for a caller that wants to find "the input
tensor" without hardcoding an index.

### Why FNV-1a and not a CRC or a signature

The `checksum` word is FNV-1a over every payload byte. The intent is
**corruption detection, not authenticity** -- catching a truncated flash write
or a bit-rotted OSPI sector before the NPU is pointed at nonsense.

- **vs CRC32:** FNV-1a is a few lines of code with no lookup table, and the
  blob is validated once at load rather than per packet. CRC32's better burst
  detection does not justify a table on a device counting kilobytes.
- **vs a signature:** a model is not code the CPU executes, and signing is
  @ref md_docs_2formats_2ROT1's job. If a `.npub` needs to be *trusted* rather
  than merely checked for corruption, the correct move is to place it inside a
  signed image -- not to grow a second, weaker crypto scheme here.

Be clear about what this buys: FNV-1a stops accidents. It stops no attacker at
all, since anyone who can modify the payload can recompute the digest.

### Why this header is not gated behind `RA8_HAS_NPU`

`ra8_npu_blob.h` defines a pure **data format** -- constants plus one
endianness-safe accessor -- and touches no NPU register. Gating it behind
`RA8_HAS_NPU` would stop the host-side generator's tests and the `board_sim`
model from including it on a device that is not an RA8P1, which is exactly where
you most want to validate a container. The RA8P1-only piece is the *loader*,
which turns a blob into an `ra8_npu_job_t`. Format definitions travel
everywhere; hardware drivers do not.

---

## 3. Wire format

All fields are little-endian `uint32_t` words. Offsets are absolute from byte 0
of the blob.

```
  +--------------------------------------------------+  offset 0
  |  header            8 words = 32 bytes            |
  +--------------------------------------------------+  offset 32
  |  region table      region_count x 16 bytes       |
  +--------------------------------------------------+
  |  command stream    cmd_bytes at cmd_offset       |
  +--------------------------------------------------+
  |  baked region data (weights, const inputs)       |
  +--------------------------------------------------+  total_bytes
```

The command stream and baked data are located by explicit offsets rather than by
adjacency, so a producer may order or pad them as alignment requires.

### 3.1 Header (32 bytes, 8 words, at offset 0)

| Word | Offset | Field | Meaning and valid range |
|------|--------|-------|-------------------------|
| 0 | 0 | `magic` | `k_ra8_npu_blob_magic` = `0x3155504E`. Little-endian bytes `4e 50 55 31`, so a hexdump reads `NPU1` **forwards**. |
| 1 | 4 | `version` | `k_ra8_npu_blob_version` = `1`. Bumped on any incompatible layout change. |
| 2 | 8 | `total_bytes` | Whole blob length. Must equal the real buffer length. |
| 3 | 12 | `region_count` | Number of region descriptors that follow. |
| 4 | 16 | `cmd_offset` | Byte offset of the Vela command stream. |
| 5 | 20 | `cmd_bytes` | Command-stream length. Must be `> 0`. |
| 6 | 24 | `accel` | `ra8_npu_blob_accel_t` -- the Vela accelerator config. Informational. |
| 7 | 28 | `checksum` | FNV-1a over every payload byte (everything at or beyond offset 32). |

Word indices are `ra8_npu_blob_word_t`; `k_ra8_npu_blob_header_bytes` = 32.

The checksum deliberately covers the payload only, not the header. The header
holds the checksum itself, so including it would be self-referential; the
header's own integrity is established by the magic, the version, and the
requirement that `total_bytes` match the real buffer length.

### 3.2 Region descriptors (16 bytes each, 4 words)

| Word | Offset | Field | Meaning |
|------|--------|-------|---------|
| 0 | 0 | `role` | `ra8_npu_blob_role_t`: `0` weights, `1` scratch, `2` input, `3` output, `4` other. Informational. |
| 1 | 4 | `flags` | `ra8_npu_blob_rflag_t`; `k_ra8_npu_blob_rflag_baked` marks the region's bytes as present in the blob. |
| 2 | 8 | `size` | Region size in bytes. |
| 3 | 12 | `data_offset` | Byte offset of the baked bytes when BAKED; unused otherwise. |

`k_ra8_npu_blob_region_desc_bytes` = 16. A descriptor's index in this table is
its `BASEPn` slot, which is how the command stream addresses it.

### 3.3 Alignment

`k_ra8_npu_blob_arena_align` = **16 bytes** -- the alignment the loader applies
to each runtime region's base within the caller's arena, because Ethos-U55
tensor arenas are 16-byte aligned. A caller sizing an arena must therefore
allow for per-region padding, not merely the sum of the `size` fields.

---

## 4. Algorithms

### 4.1 Producing (`tools/vela/vela_gen.py`)

```
  1. Run Vela on the quantised .tflite for the target accelerator config
     -> command stream, weight blob, arena layout (BASEPn descriptors)
  2. Assign each arena a role and a BAKED/RUNTIME flag
     (weights BAKED; scratch/activations RUNTIME)
  3. Lay out: header, region table, command stream, baked data
     -> resolves cmd_offset and every BAKED data_offset
  4. Compute FNV-1a over everything at or beyond offset 32
  5. Write the header with total_bytes and checksum filled in
```

Sizes are all known before any bytes are written, so -- as with RBKC and RCBZ --
the file is emitted in one forward pass with the tables at the front.

### 4.2 Loading

```
  1. Check buffer length >= k_ra8_npu_blob_header_bytes (32).
  2. magic == k_ra8_npu_blob_magic, else reject.
  3. version == k_ra8_npu_blob_version, else reject.
  4. total_bytes == the real buffer length, else reject.
  5. Region table fits: 32 + region_count * 16 <= total_bytes.
  6. Command stream lies inside the blob:
     cmd_offset + cmd_bytes <= total_bytes, and cmd_bytes > 0.
  7. FNV-1a over the payload == checksum, else reject.
  8. For each descriptor:
       BAKED   -> require data_offset + size <= total_bytes;
                  bind pointer = blob + data_offset (in place, no copy)
       RUNTIME -> align the arena cursor to 16; require the region fits
                  the remaining arena; bind pointer = arena + cursor
  9. Point the NPU at cmd_offset with the bound BASEPn table.
```

Steps 5, 6 and 8 are the memory-safety checks: every offset read from the file
is validated against `total_bytes` *before* it is turned into a pointer. Step 8's
arena check is the one that stops a hostile `size` from running the runtime
cursor past the caller's buffer.

---

## 5. Memory behaviour

The split is the point:

| Region kind | Where the bytes live | Resident SRAM cost |
|-------------|----------------------|--------------------|
| BAKED (weights) | inside the blob, used **in place** | **zero** -- executes from XIP OSPI / MRAM |
| RUNTIME (scratch, activations) | caller-supplied NPU-visible arena | `sum(size) + 16-byte alignment padding` |
| command stream | inside the blob, read by the NPU | zero |
| loader bookkeeping | `ra8_npu_job_t` + BASEPn pointer table | tens of bytes |

For a typical quantised vision model with 2 MB of weights and 256 KB of
activations, the resident SRAM cost is the **256 KB**, not 2.25 MB. The weights
stay in flash, which is what makes a model of that size viable on a device with
1.6 MB of SRAM at all.

Zero dynamic allocation (NASA P10 Rule 3): the caller owns the arena and passes
it in; the loader only computes offsets within it. The descriptor walk is
bounded by the validated `region_count` (Rule 2).

---

## 6. Worked example -- annotated layout

A `.npub` for a two-arena model (baked weights plus a runtime scratch arena)
lays out like this. Header words, in order:

```
00000000  4E 50 55 31 01 00 00 00 00 40 12 00 02 00 00 00  |NPU1.....@......|
          ^^^^^^^^^^^ ^^^^^^^^^^^ ^^^^^^^^^^^ ^^^^^^^^^^^
          magic       version=1   total=0x124000  region_count=2
00000010  60 00 00 00 20 03 00 00 00 00 00 00 A1 3C 7E 2B  |`... ........<~+|
          ^^^^^^^^^^^ ^^^^^^^^^^^ ^^^^^^^^^^^ ^^^^^^^^^^^
          cmd_off=0x60 cmd_bytes=0x320 accel=0   checksum
```

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0-3 | `4E 50 55 31` | `magic` | `0x3155504E` -- ASCII column reads **`NPU1`**, forwards |
| 4-7 | `01 00 00 00` | `version` | **1** |
| 8-11 | `00 40 12 00` | `total_bytes` | `0x124000` = **1 196 032** bytes |
| 12-15 | `02 00 00 00` | `region_count` | **2** |
| 16-19 | `60 00 00 00` | `cmd_offset` | `0x60` = **96** |
| 20-23 | `20 03 00 00` | `cmd_bytes` | `0x320` = **800** |
| 24-27 | `00 00 00 00` | `accel` | config 0, informational |
| 28-31 | `A1 3C 7E 2B` | `checksum` | FNV-1a over bytes 32..`total_bytes` |

The region table follows at offset 32, two descriptors of 16 bytes:

```
00000020  00 00 00 00 01 00 00 00 00 00 12 00 80 03 00 00  |................|
          role=0        flags=BAKED   size=0x120000  data_off=0x380
          (weights)                   = 1 179 648 B

00000030  01 00 00 00 00 00 00 00 00 00 04 00 00 00 00 00  |................|
          role=1        flags=0       size=0x40000   data_off unused
          (scratch)     (RUNTIME)     = 262 144 B
```

Reading the layout off those numbers:

- **Region table** occupies `32 .. 32 + 2*16 = 64`.
- **Command stream** at `cmd_offset` 96, 800 bytes, ending at 896 --
  comfortably inside `total_bytes`, as step 6 of the load requires.
- **Baked weights** at `data_offset` `0x380` = 896 (immediately after the
  command stream), 1 179 648 bytes, ending at `896 + 1179648 = 1180544`.
- **Scratch** has `k_ra8_npu_blob_rflag_baked` clear, so `data_offset` is
  meaningless and its 262 144 bytes come from the caller's arena.

**The memory story in one line:** the blob is ~1.2 MB and almost all of it is
weights that are never copied. Resident SRAM for this model is the **256 KB**
scratch arena -- so the file is bigger than the chip's entire SRAM, yet loads
and runs.

Cross-checks a loader performs on these exact numbers: `32 + 2*16 <= total`;
`96 + 800 <= total`; `896 + 1179648 <= total`. All hold, so no descriptor turns
into an out-of-bounds pointer.

> The byte values above are an annotated layout illustrating the specified
> field order, not a dump of a committed fixture -- unlike the JOF, RBKC, RCBZ
> and ROT1 pages, whose hexdumps are captured from real generated files. A
> `.npub` requires the Vela compiler and an RA8P1 target, neither of which is
> available in this checkout. The field offsets, sizes and constants are taken
> directly from `ra8_npu_blob.h`; regenerate this section from a real
> `vela_gen.py` output when an RA8P1 toolchain is at hand.

---

## 7. Edge cases, failure modes and security

A `.npub` is normally shipped with the firmware rather than arriving from
removable media, so it is less exposed than a book or a comic. The loader is
nonetheless fail-closed, because "normally" is not "always" -- a model can be
side-loaded for evaluation.

| Malformed input | What could go wrong | What actually happens |
|-----------------|---------------------|-----------------------|
| Buffer shorter than 32 bytes | Header read past the end | Length checked before any field is read |
| Wrong `magic` | Foreign buffer mis-parsed | Rejected at the first check |
| Wrong `version` | Fields read at the wrong offsets | Rejected; unknown revisions are never best-effort parsed |
| `total_bytes` != real buffer length | Every bound check made meaningless | Cross-checked against the caller's length |
| `region_count` huge | Descriptor walk past the end | `32 + region_count * 16 <= total_bytes` |
| `cmd_bytes` = 0 | NPU pointed at an empty stream | Must be `> 0` |
| `cmd_offset + cmd_bytes` > `total_bytes` | NPU reads past the blob | Checked before the stream is bound |
| BAKED `data_offset + size` > `total_bytes` | Weight pointer out of bounds | Checked per descriptor |
| RUNTIME `size` larger than the arena | Arena overflow into other SRAM | Checked against remaining arena, after 16-byte alignment |
| Bit rot in the payload | NPU executes corrupt commands | FNV-1a mismatch rejects the blob |
| Deliberately modified payload with recomputed checksum | Attacker-chosen command stream | **Not defended** -- see below |
| Unknown `role` | Loader mis-binds an arena | Harmless: `role` is informational; binding is by index |
| Overlapping baked regions | Two regions alias | Legal on the wire; each is independently bounded, so it is a producer bug, not a safety issue |

### What is deliberately *not* defended

- **Authenticity.** FNV-1a is not cryptographic. Anyone who can modify a
  `.npub` can recompute it. NPU1 detects corruption, never tampering. A model
  that must be trusted belongs inside a signed image
  (see @ref md_docs_2formats_2ROT1).
- **Command-stream semantics.** The loader validates that the stream lies inside
  the blob; it does not interpret it. A structurally valid stream that instructs
  the NPU to compute nonsense will compute nonsense. Constraining what the NPU
  may *address* is the memory system's job, via the BASEPn bindings the loader
  set up.
- **Model correctness.** A blob that loads cleanly may still be a badly-trained
  model. Not a format concern.

---

## 8. Versioning

NPU1 carries **both** a magic and an explicit `version` field, like
@ref md_docs_2formats_2ROT1 and for the same reason: the layout is expected to
change as Vela and the accelerator configuration evolve, so a numeric field is
the right instrument.

```
   magic = "NPU1"          version = 1
           \__/ |                    |
            |   |                    |
          family|            layout revision -- the field that moves
                |
           revision hint (spend a new one only if the
           header itself is restructured)
```

- `version` is checked **before any offset in the file is interpreted**, so a
  future v2 blob is refused by a v1 loader rather than read with the wrong word
  indices.
- The `accel` word is not a version. It records which Vela accelerator
  configuration the stream was compiled for. Running a stream on a
  differently-configured NPU is a compatibility question the loader reports on
  rather than silently accepting.
- `role` and `flags` are the intended growth points: new roles and new region
  flags extend the format without touching the header layout. A loader that
  meets an unknown `role` keeps working, since binding is by index.

Under the zero-backward-compatibility policy: bump `k_ra8_npu_blob_version`,
update `vela_gen.py` and the loader in the same commit, regenerate every blob,
and delete the old path. There is no dual-version loader.

---

## See also

- `ra8_npu_blob.h` -- the format constants and word indices specified here
- `ra8_npu_loader.h` -- turns a validated blob into an `ra8_npu_job_t`
- `tools/vela/vela_gen.py` -- the producing tool
- @ref md_docs_2formats_2ROT1 -- the signed wrapper to use when a model must be
  authentic and not merely uncorrupted
- @ref md_docs_2formats_2BINARY__FORMATS -- why this magic reads forwards
