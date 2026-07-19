# NSR1 -- The Non-Secure Image RoT Header

**Magic:** `NSR1` (`0x3152534E`) &nbsp;|&nbsp;
**Library:** `libs/ra8_tz_secure_boot` &nbsp;|&nbsp;
**Producer:** `src/app/ns_image.ld` (the NS linker script) &nbsp;|&nbsp;
**Size:** 8 bytes, fixed, at NS base + `0x40`

---

## 1. Synopsis

NSR1 is the smallest format in this section -- two `uint32` fields -- and it
exists to answer a single question at the TrustZone boundary:

> The Secure world is about to hand control to the Non-Secure image. It must
> verify that image first. **Where does the signed body end?**

Verifying a @ref md_docs_2formats_2ROT1 trailer requires knowing `body_len`,
because the trailer lives at `image_base + body_len`. For a DFU payload that
length arrives in the DFU header. But a Non-Secure application is not delivered
through DFU -- it is simply *there*, flashed into the NS MRAM partition, and the
Secure world must locate its trailer with nothing but the NS base address.

The obvious answers are all bad:

- **Hardcode the trailer address in Secure code.** Now every NS image must be
  exactly one size, or the Secure world must be rebuilt whenever the NS image
  grows. A cross-image constant that changes with every build is a bug waiting
  to happen.
- **Scan for the `ROT1` magic.** An unbounded search over untrusted memory, and
  trivially confused by the bytes `31 54 4F 52` occurring inside the image.

NSR1 is the third answer: the NS image **describes itself**. The NS linker
script emits an 8-byte record at a fixed, deterministic offset -- a magic and
the signed body length -- and the Secure verifier reads it to learn where the
trailer begins. The whole cross-image contract is one constant, `0x40`, and the
linker `ASSERT`s that it holds.

If you stop reading here: *NSR1 is an 8-byte self-describing header the NS
linker embeds at NS base + 0x40, carrying a magic and the signed body length, so
the Secure world can locate the ROT1 trailer without a hardcoded address or an
unbounded scan.*

---

## 2. Design rationale

### Why `0x40`, and why that constant is load-bearing

The Armv8-M Non-Secure vector table occupies the very start of the image: 16
slots at 4 bytes each = 64 bytes = `0x40`. So `0x40` is the **first free,
deterministic offset** from the NS base -- immediately after the vectors, before
any code.

That makes the constant `k_ra8_tz_ns_rot_header_offset = 0x40` the entire
cross-image contract between three independently-built things: the NS linker
script, the Secure verifier, and the signing tool. It also means **the NS vector
table must remain exactly 16 entries**. Add a 17th and the header moves, the
Secure world reads the wrong 8 bytes, and the magic check denies the boot.

That is a nasty class of bug -- a silent layout drift discovered only as "the
device stopped booting" -- so it is not left to convention. `ns_image.ld`
asserts it at link time:

```
ASSERT(g_ra8_ls_ns_rot_header - g_ra8_ls_ns_run_start == 0x40,
       "FATAL: .ns_rot_header not at ns_base + 0x40 (NS vector table must be 16 entries)")
```

The build fails rather than producing an unbootable image. This is the pattern
worth taking away: when a constant is shared across separately-linked images,
**make one side assert it** -- a comment saying "keep these in sync" is not a
mechanism.

### The comparison that makes it click

@dot
digraph nsr1_ways {
  bgcolor="transparent";
  rankdir=TB;
  fontname="Helvetica";
  node [shape=box, fontname="Helvetica", fontsize=10, style="filled,rounded"];
  edge [fontname="Helvetica", fontsize=9];

  subgraph cluster_hard {
    label="A: hardcode the trailer address in Secure code";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b06a6a"; style="rounded"; bgcolor="#fbeeee";
    h0 [label="Secure world knows\ntrailer @ 0x0208_0000", fillcolor="#e8c9c9", color="#b06a6a"];
    h1 [label="NS image grows by one function", fillcolor="#e8c9c9", color="#b06a6a"];
    h2 [label="trailer moves; Secure world reads\nrandom code as a trailer\n-> rebuild BOTH images in lockstep,\n   forever", fillcolor="#e8a8a8", color="#8c3a3a", width=4];
    h0 -> h1 -> h2;
  }

  subgraph cluster_scan {
    label="B: scan the NS image for the ROT1 magic";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b8913f"; style="rounded"; bgcolor="#fbf6ea";
    s0 [label="search 512 KiB of untrusted memory\nfor bytes 31 54 4F 52", fillcolor="#f5e6c8", color="#b8913f", width=4];
    s1 [label="unbounded loop (NASA P10 Rule 2),\nand any image containing those\nfour bytes hijacks the search", fillcolor="#efd9a8", color="#b8913f"];
    s0 -> s1;
  }

  subgraph cluster_self {
    label="C: NSR1 -- the image describes itself at a fixed offset";
    fontsize=11; fontname="Helvetica-Bold";
    color="#5f9e72"; style="rounded"; bgcolor="#eef7f0";
    n0 [label="read 8 bytes at ns_base + 0x40", fillcolor="#dff0e4", color="#5f9e72"];
    n1 [label="magic ok -> trailer @ ns_base + body_len\n(one load, no search, no shared constant\nbeyond 0x40 -- which the linker ASSERTs)",
        fillcolor="#cfe8d6", color="#5f9e72", width=4];
    n0 -> n1;
  }
}
@enddot

### Why trusting `body_len` from an untrusted image is safe

This is the part that looks wrong at first glance and is worth understanding,
because the reasoning generalises.

The NS image is **untrusted** -- that is the entire premise of verifying it. So
why is it acceptable to read a length out of it and use that length to decide
what to hash?

Because **lying about it gains nothing**. `body_len` only selects *which bytes
get hashed*. An attacker who changes it changes the hash input, which changes
the digest, which means the ECDSA signature over that material no longer
verifies. There is no value of `body_len` that produces a valid signature
without the private key. A wrong length simply default-denies at the signature
check.

The general principle: **a field read from untrusted data is safe to trust when
a downstream cryptographic check already covers every outcome it can produce.**
What still must be bounded is *memory safety* -- a huge `body_len` must not read
out of bounds -- and that is handled by `k_ra8_rot_body_max` (1 MiB) capping the
hash loop, plus the NS partition being 512 KiB.

Note also that the header word itself lies **inside** the signed body, so
`body_len` is covered by the signature: a valid image cannot disagree with its
own header.

---

## 3. Wire format

Little-endian, 8 bytes, at `ns_vector_table + k_ra8_tz_ns_rot_header_offset`
(`0x40`).

```
  ns_base + 0x00  +----------------------------------+
                  |  NS vector table, 16 x 4 bytes   |
  ns_base + 0x40  +----------------------------------+
                  |  magic     u32   "NSR1"          |
  ns_base + 0x44  +----------------------------------+
                  |  body_len  u32                   |
  ns_base + 0x48  +----------------------------------+
                  |  ... NS code and data ...        |
  ns_base+body_len+----------------------------------+
                  |  ROT1 trailer, 116 bytes         |
                  +----------------------------------+
```

| Offset | Size | Field | Meaning and valid range |
|--------|------|-------|-------------------------|
| 0 | 4 | `magic` | `k_ra8_tz_ns_rot_header_magic` = `0x3152534E`. Little-endian bytes `4e 53 52 31`, so a hexdump reads `NSR1` **forwards**. |
| 4 | 4 | `body_len` | Signed body length in bytes: `signed_body_end - ns_run_start`, computed by the NS linker. The ROT1 trailer begins at `ns_base + body_len`. |

The struct is `ra8_ns_rot_header_t`; the offset constant is
`k_ra8_tz_ns_rot_header_offset`.

Unlike `ROT1` and `RBK1`, the `NSR1` constant was deliberately chosen so its
little-endian bytes spell the tag in order -- convenient when eyeballing a
hexdump of a flashed NS image. See @ref md_docs_2formats_2BINARY__FORMATS.

---

## 4. Algorithms

### 4.1 Producing (the NS linker script)

`src/app/ns_image.ld` places a `.ns_rot_header` section at `ALIGN(4)`
immediately after the vector table, emits the magic word and a `body_len`
computed from the linker's own symbols (`signed_body_end - ns_run_start`), and
then asserts the placement. No post-processing step is involved: the header is a
product of linking, so it cannot drift from the image it describes.

`tools/rot_sign.py` later appends the ROT1 trailer after exactly `body_len`
bytes, which is what makes `[ body ][ trailer ]` line up with what the header
claims.

### 4.2 Consuming (`ra8_tz_ns_signed_body_len()`)

```
  1. If ns_vector_table is NULL            -> return 0 (deny sentinel)
  2. Read the 8 bytes at ns_base + 0x40
  3. If magic != k_ra8_tz_ns_rot_header_magic -> return 0 (deny)
  4. Return body_len
```

The function returns `0` for every failure, and `0` is a **deny sentinel**: a
caller that gets 0 must not `BLXNS`. Collapsing "no header", "wrong magic" and
"null pointer" into one refusing value is deliberate -- there is no partial
success to represent, and a single sentinel is harder to mishandle than a set of
distinguishable error codes the caller might accidentally treat as advisory.

The returned length then feeds `ra8_rot_trailer_after()`, which locates the
ROT1 trailer, and the boot proceeds through the full verification flow in
@ref md_docs_2formats_2ROT1 before any `BLXNS` executes.

---

## 5. Memory behaviour

Nothing is resident. The header is two words read directly from the NS image in
place -- no copy, no buffer, no allocation. The cost of consuming NSR1 is a
bounds check and two loads.

Its *purpose*, however, is to enable the streaming verification described in
@ref md_docs_2formats_2ROT1: the NS body is hashed incrementally and never held
in RAM, so a 512 KiB NS image is verified in well under a kilobyte of working
set -- which matters because this happens in the Secure world, where the budget
is tightest, before any NS code is trusted enough to run.

---

## 6. Worked example -- real bytes

An NS image built with the RoT header begins like this (the first 0x48 bytes):

```
00000000  00 80 21 22 C1 03 02 00 ... (16 NS vector entries, 64 bytes)
          initial SP    Reset_Handler
   ...
00000040  4E 53 52 31 00 40 07 00                          |NSR1.@..|
          ^^^^^^^^^^^ ^^^^^^^^^^^
          magic       body_len = 0x00074000 = 475136
```

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0x40-0x43 | `4E 53 52 31` | `magic` | `0x3152534E` -- ASCII column reads **`NSR1`**, forwards |
| 0x44-0x47 | `00 40 07 00` | `body_len` | `0x00074000` = **475136** bytes |

From those eight bytes the Secure world derives everything it needs:

- the ROT1 trailer starts at `ns_base + 475136`;
- the bytes to hash are `[ns_base, ns_base + 475136)`;
- `475136 < k_ra8_rot_body_max` (1 MiB), so the hash loop is bounded;
- `475136 < 512 KiB`, so the body fits the NS MRAM partition.

Contrast the ASCII column with the ROT1 example in
@ref md_docs_2formats_2ROT1, where the same style of `uint32` magic shows up as
`1TOR`. Same mechanism, opposite-looking result, purely because of how each
constant was chosen.

To confirm the placement on a real build, the link-time assertion is the
authority -- if `.ns_rot_header` were not at `+0x40`, the image would not have
linked at all.

---

## 7. Edge cases, failure modes and security

| Malformation or attack | What actually happens |
|------------------------|-----------------------|
| `ns_vector_table` is NULL | Returns `0` -> caller must not `BLXNS` |
| No RoT header (NS built without it) | Magic mismatch -> `0` -> **deny**. Absence is never a bypass. |
| Corrupt magic | Deny |
| NS vector table grown past 16 entries | Link-time `ASSERT` fails -- the image never gets built |
| `body_len` forged larger | Hash covers different bytes -> digest differs -> ECDSA verify fails -> deny. Bounded by `k_ra8_rot_body_max`, so no out-of-bounds read. |
| `body_len` forged smaller | Same: different hash input -> signature invalid -> deny |
| `body_len` = 0 | ROT1's `body_len >= 1` check denies |
| Header edited to point the trailer at attacker-controlled bytes | Those bytes must contain a valid ECDSA-P256 signature over the material the forged length implies -- unforgeable without the private key |
| Header itself modified | It lies inside the signed body, so any edit invalidates the signature |

The recurring theme: **NSR1 carries no security burden of its own.** It is a
locator, and every value it can produce is downstream of a signature check that
fails closed. Its only hard requirement is memory safety -- never read outside
the NS partition -- which the 1 MiB body cap and the partition size enforce.

### What is deliberately *not* defended

- **Authenticity of the header alone.** There is no separate check on the 8
  bytes; they are covered by the ROT1 signature over the body they sit in.
- **Anything after `BLXNS`.** NSR1 participates in deciding whether NS code runs.
  What that code then does is TrustZone's, the SAU's and the MPU's problem.

---

## 8. Versioning

The fourth byte `1` is the revision, following the discriminator-byte scheme in
@ref md_docs_2formats_2BINARY__FORMATS. There is no separate `version` field --
at 8 bytes with two fields, a version field would be a quarter of the format,
and there is no plausible compatible extension to negotiate.

A layout change spends a new magic (`NSR2`). Because a mismatched magic returns
the deny sentinel, an old Secure world confronted with a new NS image refuses to
branch rather than misreading the header -- the correct failure direction for a
boot decision, and it costs no code to get.

Three things must move together for any change here: `ns_image.ld` (producer),
`ra8_tz_secure_boot.h` / its verifier (consumer), and the `0x40` assertion. The
`ASSERT` is what makes that safe -- a change that breaks the contract fails the
build instead of the boot.

---

## See also

- `ra8_tz_secure_boot.h` -- `ra8_ns_rot_header_t`, the offset constant, and
  `ra8_tz_ns_signed_body_len()`
- `src/app/ns_image.ld` -- the producing linker script and its placement assertion
- @ref md_docs_2formats_2ROT1 -- the trailer this header locates, and the
  verification flow that consumes both
- @ref md_docs_2formats_2BINARY__FORMATS -- why this magic reads forwards and
  `ROT1` does not
