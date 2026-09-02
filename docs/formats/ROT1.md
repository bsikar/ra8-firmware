# ROT1 -- The Root-of-Trust Signed-Image Trailer

**Magic:** `ROT1` (`0x524F5431`) &nbsp;|&nbsp;
**Library:** `libs/ra8_dfu` &nbsp;|&nbsp;
**Producer:** `tools/rot/src/rot_sign.py` &nbsp;|&nbsp;
**Size:** 116 bytes, fixed

---

## 1. Synopsis

ROT1 is the format that answers one question: **may this code be allowed to
run?**

Every other format in this section is defended against *accidents and
malformation* -- a truncated file, a corrupt index, a decompression bomb. Their
threat model explicitly stops short of an attacker with intent. ROT1 is where
that changes. It is the only first-party format here whose purpose is
**authenticity**, and it is the thing standing between a device that runs the
firmware you signed and a device that runs firmware someone else wrote.

The shape is deliberately boring, because boring is what you want here. Take an
image -- a DFU update payload, or a Non-Secure application -- and append a
fixed 116-byte record after it:

```
  [ ................ body (body_len bytes) ................ ][ 116-byte ROT1 trailer ]
```

The trailer carries a **SHA-256 digest** of the body and an **ECDSA-P256
signature** produced with a private key that never exists on the device. The
verifier holds only the corresponding public key, compiled into secure-world
code. It re-hashes the body, checks the signature, and default-denies on any
failure -- wrong magic, wrong version, bad length, bad digest, bad signature, or
an image version older than the one already installed.

That last clause is the second job: `img_version` is a **monotonic
anti-rollback counter**. Signing an old image correctly is not enough; an
attacker who captures a genuinely-signed but *outdated* image (one with a
patched vulnerability) must not be able to install it. And crucially,
`img_version` is itself covered by the signature -- so raising the field to
defeat the rollback check simply invalidates the signature.

If you stop reading here: *ROT1 is a fixed 116-byte authenticity trailer --
SHA-256 digest plus ECDSA-P256 signature plus a signature-covered monotonic
version counter -- appended after an image body, verified in the secure world
against a compiled-in public key, default-denying on every failure.*

---

## 2. Design rationale

### Why a trailer and not a header

Putting the signature *after* the body looks unusual until you consider what
signing actually is: a post-processing step on an already-linked binary.

- A **header** would sit inside the address range the linker script assigns, so
  reserving space for it means every image must be built knowing it will be
  signed, and the signing tool must patch bytes in the middle of a laid-out
  image. Get the reservation wrong and you relink.
- A **trailer** is pure concatenation. `rot_sign.py` reads a finished
  `body.bin`, computes over it, and writes `body.bin || trailer`. Signing never
  touches the image's internal layout, so an unsigned and a signed build of the
  same firmware are byte-identical up to `body_len`.

The cost is that the verifier must know where the body ends. For a DFU payload
that comes from the DFU header's length word; for a Non-Secure image it comes
from @ref md_docs_2formats_2NSR1, an 8-byte header the NS linker embeds
precisely so the trailer can be located without a hardcoded address.

### The comparison that makes it click

The tempting cheap alternative is a checksum -- a CRC32 or a bare SHA-256 with
no signature. Both "detect corruption". Only one resists an attacker:

@dot
digraph checksum_vs_sig {
  bgcolor="transparent";
  rankdir=TB;
  fontname="Helvetica";
  node [shape=box, fontname="Helvetica", fontsize=10, style="filled,rounded"];
  edge [fontname="Helvetica", fontsize=9];

  subgraph cluster_ck {
    label="Checksum / bare digest: anyone who can write the image can write the check";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b06a6a"; style="rounded"; bgcolor="#fbeeee";
    k0 [label="attacker modifies the body", fillcolor="#e8c9c9", color="#b06a6a"];
    k1 [label="attacker recomputes SHA-256\n(a public algorithm --\nno secret involved)", fillcolor="#e8c9c9", color="#b06a6a", width=4];
    k2 [label="attacker writes the new digest\ninto the trailer", fillcolor="#e8c9c9", color="#b06a6a"];
    k3 [label="verifier: digest matches!\nMALICIOUS IMAGE BOOTS", fillcolor="#e8a8a8", color="#8c3a3a"];
    k0 -> k1 -> k2 -> k3;
  }

  subgraph cluster_sig {
    label="ROT1: verifying and producing need DIFFERENT keys";
    fontsize=11; fontname="Helvetica-Bold";
    color="#5f9e72"; style="rounded"; bgcolor="#eef7f0";
    s0 [label="attacker modifies the body", fillcolor="#dff0e4", color="#5f9e72"];
    s1 [label="attacker recomputes SHA-256\n-- fine, that part is public", fillcolor="#dff0e4", color="#5f9e72"];
    s2 [label="attacker must now SIGN it\nwith the P-256 private key\n-- which is not on the device\nand never was", fillcolor="#cfe8d6", color="#5f9e72", width=4];
    s3 [label="verifier: signature invalid\nBOOT DENIED", fillcolor="#a8d8b6", color="#3a7c50"];
    s0 -> s1 -> s2 -> s3 [minlen=1];
  }
}
@enddot

The asymmetry is the whole point. A digest proves *the bytes are intact*; a
signature proves *the bytes came from whoever holds the private key*. Only the
second is a security property, because only the second involves something the
attacker cannot compute. The device holds the **public** key (65 bytes,
uncompressed P-256 point `0x04 || X || Y`, embedded in `s_rot_root_pubkey`), so
extracting the firmware yields nothing that helps forge an image.

### Why ECDSA-P256 and not RSA or Ed25519

- **vs RSA-2048:** a P-256 signature is 64 bytes against RSA's 256, and the
  public key is 65 bytes against ~270. On a device where the trailer is
  appended to every image and the key sits in secure MRAM, a 4x size reduction
  at equivalent security is decisive. Verification is also far cheaper on a
  Cortex-M.
- **vs Ed25519:** genuinely attractive, and slightly faster. P-256 wins on
  ecosystem: `openssl` supports it everywhere, which lets `rot_sign.py` shell
  out rather than depend on a third-party Python crypto package -- and a
  release-signing tool with no pip dependencies is a tool that still works in
  five years. P-256 is also the curve the hardware and PSA ecosystem support
  natively.

### Why `img_version` is inside the signed material

The naive design signs only the body digest and stores `img_version` as
plaintext metadata. That is broken, and subtly so.

An attacker who holds *any* validly-signed old image -- say v3, with a known
vulnerability -- can simply edit the plaintext `img_version` field to 99. The
signature still verifies (it only covered the body, which is unmodified), the
anti-rollback check sees 99 > current, and the vulnerable image installs. The
rollback protection has been defeated without breaking any crypto.

So the signature covers **`SHA-256(img_version_le || body_digest)`**, not the
bare body digest. Raising `img_version` changes the signed material and
invalidates the signature. `digest` itself still covers only the body, so it
remains a usable standalone integrity check. `rot_sign.py` binds identical
material, and the two must be changed together or verification fails closed --
which is the correct way for that coupling to break.

### Why the body length is capped at 1 MiB

`k_ra8_rot_body_max` = `0x00100000`. The hash loop runs over `body_len` bytes,
and `body_len` is read from an untrusted image. Without a cap, a corrupt or
hostile length word drives an unbounded read (NASA P10 Rule 2 violation) into
whatever follows in the address space. 1 MiB comfortably exceeds both the
largest DFU slot image (~448 KiB) and the Non-Secure MRAM partition (512 KiB),
so the cap costs nothing real and bounds the loop absolutely.

---

## 3. Wire format

All integers are **little-endian**. The trailer begins at `image_base +
body_len` and is exactly **116 bytes**. Every field is 32-bit aligned.

```
  +--------------------------------------------------+  body_len + 0
  |  magic         u32   "ROT1"  (0x524F5431)        |
  +--------------------------------------------------+  + 4
  |  version       u32   trailer format revision     |
  +--------------------------------------------------+  + 8
  |  img_version   u32   monotonic anti-rollback     |
  +--------------------------------------------------+  + 12
  |  body_len      u32   bytes the digest covers     |
  +--------------------------------------------------+  + 16
  |  sig_len       u32   active signature length     |
  +--------------------------------------------------+  + 20
  |  digest[32]          SHA-256(body)               |
  +--------------------------------------------------+  + 52
  |  sig[64]             ECDSA-P256, raw r || s      |
  +--------------------------------------------------+  + 116
```

| Offset | Size | Field | Meaning and valid range |
|--------|------|-------|-------------------------|
| 0 | 4 | `magic` | `k_ra8_rot_trailer_magic` = `0x524F5431`. On disk the little-endian bytes are `31 54 4f 52`, which a hex viewer shows as `1TOR` -- see section 6. |
| 4 | 4 | `version` | `k_ra8_rot_version` = `1`. Trailer *format* revision. |
| 8 | 4 | `img_version` | Monotonic image version for anti-rollback. Covered by `sig`. |
| 12 | 4 | `body_len` | Bytes the digest covers. `1 .. k_ra8_rot_body_max` (1 MiB). |
| 16 | 4 | `sig_len` | Active signature length. `<= k_ra8_rot_sig_bytes` (64). |
| 20 | 32 | `digest` | `SHA-256(body)`, over exactly `body_len` bytes. |
| 52 | 64 | `sig` | ECDSA-P256 signature, raw `r || s` (two 32-byte big-endian field elements), over `SHA-256(img_version_le \|\| digest)`. |

`sig_len` exists because the same struct serves two builds: 64 bytes for a real
ECDSA-P256 signature on target, and a 32-byte stand-in under
`RA8_OFF_TARGET` so host tests can exercise the plumbing without a real
key. The unused tail of `sig` is ignored. It is **not** a negotiation
mechanism: a target build requires the full 64.

The related public key is not in the trailer. It is 65 bytes
(`k_ra8_rot_pubkey_bytes`), the uncompressed P-256 point `0x04 || X || Y`,
compiled into `s_rot_root_pubkey` in secure-world code. Shipping the key with
the image it validates would be circular; the whole point is that the key is
part of the device, not part of the update.

---

## 4. Algorithms

### 4.1 Signing (`tools/rot/src/rot_sign.py`)

```
  1. Read body.bin  -> body, body_len
  2. Reject body_len > k_ra8_rot_body_max (1 MiB)
  3. digest      = SHA-256(body)
  4. signed_mat  = SHA-256( u32le(img_version) || digest )
  5. sig         = ECDSA-P256-sign(private_key, signed_mat)   -> raw r||s, 64 B
  6. trailer     = pack(magic, version, img_version, body_len, 64, digest, sig)
  7. Write body || trailer
```

The tool shells out to `openssl` for steps 3-5, so it needs no third-party
Python packages. It is a **build/release-time host tool and never a CI gate** --
CI has no access to the private key, by design.

`rot_sign.py selftest` generates a throwaway key, signs a random body,
re-verifies with `openssl`, and checks the trailer layout -- proving the
produced signature really is valid ECDSA-P256 over SHA-256 with a 116-byte
trailer.

### 4.2 Verifying (`ra8_rot_verify_image()`)

@dot
digraph rot_verify {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
        fillcolor="#e8eef7", color="#5a7ca6"];
  edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
  deny [shape=box, style="rounded,filled", fillcolor="#f7dcdc", color="#b06a6a",
        label="DENY\n(do not run the image)"];
  ok   [shape=box, style="rounded,filled", fillcolor="#d6efd9", color="#5f9e72",
        label="ALLOW\n(branch / commit the update)"];

  a [label="locate trailer at base + body_len"];
  b [label="magic == ROT1 ?"];
  c [label="version == 1 ?"];
  d [label="1 <= body_len <= 1 MiB ?\nsig_len <= 64 ?"];
  e [label="recompute SHA-256 over body\n== digest ?"];
  f [label="ECDSA-P256-verify(pubkey, sig,\n  SHA-256(img_version || digest)) ?"];
  g [label="img_version >= stored counter ?"];

  a -> b -> c -> d -> e -> f -> g -> ok;
  b -> deny [label="no", color="#b06a6a", fontcolor="#b06a6a"];
  c -> deny [label="no", color="#b06a6a", fontcolor="#b06a6a"];
  d -> deny [label="no", color="#b06a6a", fontcolor="#b06a6a"];
  e -> deny [label="no", color="#b06a6a", fontcolor="#b06a6a"];
  f -> deny [label="no", color="#b06a6a", fontcolor="#b06a6a"];
  g -> deny [label="no", color="#b06a6a", fontcolor="#b06a6a"];
}
@enddot

Two properties of that flow are worth naming:

- **Every edge that is not "all checks passed" leads to DENY.** There is no path
  that treats an unrecognised trailer as "probably fine", and no warn-only
  mode. Absence of a trailer is denial, not a bypass -- which is why
  `RA8_ENABLE_ROOT_OF_TRUST` could not be turned on at all until `rot_sign.py`
  existed to *produce* trailers.
- **The order is deliberate.** Cheap structural checks (magic, version, bounds)
  run before the expensive hash, and the hash runs before the far more expensive
  signature verification. A malformed image is rejected in nanoseconds; only a
  structurally plausible one costs a full ECDSA verify. This is a
  denial-of-service consideration, not just an optimisation.

### 4.3 Anti-rollback

The monotonic counter check lives in `ra8_dfu_antirollback.h`. Verification
proves the image is *authentic*; the counter proves it is *not older than what
is installed*. Both must pass. Because `img_version` is inside the signed
material, the two checks cannot be played off against each other.

---

## 5. Memory behaviour

Verification is streaming and allocation-free:

| Cost | Size | Notes |
|------|------|-------|
| trailer | 116 bytes | read once |
| SHA-256 state | ~104 bytes | body is hashed incrementally, never resident |
| public key | 65 bytes | `.rodata` in secure world |
| ECDSA verify scratch | a few hundred bytes of stack | bounded by the implementation |

**The body is never held resident.** It is hashed in bounded blocks as it is
read, so verifying a 448 KiB DFU image costs well under a kilobyte of RAM. That
is what allows verification to happen in the secure world, where the memory
budget is tightest, and before any of the image is trusted enough to execute.

Zero dynamic allocation (NASA P10 Rule 3), and the hash loop is bounded by the
validated `body_len` (Rule 2).

---

## 6. Worked example -- real bytes

Reproducible with a throwaway key. **Never commit a private key**; this one is
generated into a temporary directory and discarded.

```
$ python3 tools/rot/src/rot_sign.py keygen --key /tmp/demo.pem
$ python3 -c "open('/tmp/body.bin','wb').write(bytes((i*7+3)&0xFF for i in range(1024)))"
$ python3 tools/rot/src/rot_sign.py sign --key /tmp/demo.pem --image /tmp/body.bin \
      --out /tmp/signed.bin --img-version 7
signed 1024 body bytes -> signed.bin (1140 bytes)
```

`1024 + 116 = 1140`. The body is untouched; the file simply got 116 bytes
longer.

### 6.1 The trailer header, byte by byte

The trailer starts at offset `body_len` = 1024 = `0x400`:

```
00000400  31 54 4F 52 01 00 00 00 07 00 00 00 00 04 00 00 |1TOR............|
00000410  40 00 00 00 E9 18 3D 9A 79 AA D8 A0 47 B8 E6 79 |@.....=.y...G..y|
```

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0-3 | `31 54 4F 52` | `magic` | `0x524F5431` = `"ROT1"` -- **note the ASCII column reads `1TOR`** |
| 4-7 | `01 00 00 00` | `version` | **1** = `k_ra8_rot_version` |
| 8-11 | `07 00 00 00` | `img_version` | **7** -- the `--img-version 7` we passed |
| 12-15 | `00 04 00 00` | `body_len` | `0x0400` = **1024** -- matches the body exactly |
| 16-19 | `40 00 00 00` | `sig_len` | `0x40` = **64** -- full ECDSA-P256 signature |
| 20-51 | `E9 18 3D 9A ...` | `digest` | SHA-256 of the body, first bytes `e9183d9a...` |
| 52-115 | (64 bytes) | `sig` | ECDSA-P256 `r \|\| s` |

**`1TOR` is not a bug.** This is the byte-order lesson from
@ref md_docs_2formats_2BINARY__FORMATS made concrete: `ROT1` is declared as the
`uint32_t` constant `0x524F5431` and compared with `==`, so on a little-endian
machine its bytes land in memory reversed. Contrast `NPU1` and `NSR1`, whose
constants were deliberately chosen so they *do* read forwards. If you are
hunting for a signed image in a hexdump, search for `1TOR`.

### 6.2 Verifying the digest independently

The `digest` field should be exactly `SHA-256(body)` -- checkable without any
project tooling:

```
$ python3 -c "import hashlib; print(hashlib.sha256(open('/tmp/body.bin','rb').read()).hexdigest())"
e9183d9a79aad8a047b8e67981210d50b01fc75b1edba5bc32ba3d3ec4d5056d
```

which matches the `digest` bytes at trailer offset 20 exactly. Note what this
does *not* prove: that the image is authentic. Anyone can recompute that digest.
The authenticity claim lives entirely in the 64 signature bytes at offset 52,
and checking those requires the public key -- which is the asymmetry from
section 2, visible in a hexdump.

### 6.3 Proving the whole scheme end to end

```
$ python3 tools/rot/src/rot_sign.py selftest
rot_sign.py selftest: PASS -- sign/verify round-trip + trailer layout OK.
```

`selftest` re-verifies the produced signature with `openssl` rather than with
the same code that produced it, so it proves interoperability with a standard
implementation and not merely self-consistency.

---

## 7. Edge cases, failure modes and security

Unlike every other format in this section, here the threat model **is** an
attacker with intent. The requirement is not "do not crash" but "do not run code
the key holder did not authorise".

| Attack or malformation | What actually happens |
|------------------------|-----------------------|
| No trailer at all | Magic check fails -> **DENY**. Absence is never a bypass. |
| Wrong `magic` | DENY at the first check |
| Wrong `version` | DENY -- an unknown trailer revision is never best-effort parsed |
| `body_len` = 0 or > 1 MiB | DENY; the cap bounds the hash loop (NASA P10 Rule 2) |
| `sig_len` > 64 | DENY; cannot over-read the fixed `sig` array |
| Body modified, digest recomputed | Signature no longer matches the body -> DENY |
| Body modified, digest **and** signature recomputed | Requires the private key. Not on the device, never was. |
| `img_version` raised to defeat rollback | `img_version` is inside the signed material -> signature invalid -> DENY |
| Old but genuinely-signed image replayed | Signature is valid, but the monotonic counter rejects it -> DENY |
| Truncated image (trailer partly missing) | Trailer read is bounded and the length checks fail -> DENY |
| Trailer present but body shorter than `body_len` | Hash covers a bounded, validated range; digest mismatch -> DENY |
| Bit flip anywhere in the body | Digest mismatch -> DENY |
| Public key extracted from the firmware | Expected and harmless -- it is public. It verifies; it cannot sign. |
| Fake stand-in signature on a target build | `RA8_OFF_TARGET` is a build-time mode, not a runtime field; a target build requires a full 64-byte ECDSA signature |

### What ROT1 does *not* protect against

Being precise about this matters more here than anywhere else in this section:

- **Compromise of the private key.** ROT1's security reduces entirely to that
  key staying secret. Its handling -- generation ceremony, storage, backup,
  re-keying -- is an operational problem, not a format problem, and the format
  cannot help if the key leaks.
- **Downgrade *within* the same `img_version`.** The counter is monotonic, not
  unique. Two images signed with the same version are interchangeable.
- **Confidentiality.** The image is signed, not encrypted. Anyone can read it.
  ROT1 makes no secrecy claim whatsoever.
- **Runtime compromise.** ROT1 validates code *before it runs*. It says nothing
  about what happens afterwards; that is TrustZone's and the MPU's job.
- **Physical attacks.** Glitching the verify branch, or reflashing MRAM with a
  debugger, are out of scope for a signature format.

---

## 8. Versioning

ROT1 is one of the two formats here that carries **both** a magic and an
explicit `version` field, and the split is intentional:

```
   magic  = "ROT1"        version = 1
            \__/ |                  |
             |   |                  |
           family|              trailer format revision
                 |              (the field you bump)
            revision hint
```

- `magic` identifies the family. A reader `memcmp`s -- strictly, `==` on the
  `uint32` -- and a mismatch is an immediate, clean denial.
- `version` is the field that actually moves. Because a signature scheme is
  expected to be revised (a curve change, an added field, a different signed
  material construction), a numeric field is the right tool: the verifier
  checks it *before* interpreting any offset, so a v2 trailer is refused by a
  v1 verifier rather than misread.

The rule for any change: **an unknown version is a denial, never a
best-effort parse.** A verifier that guessed at an unfamiliar trailer would be
a verifier an attacker could steer.

Changing the signed-material construction (as the `img_version` binding did)
requires updating `rot_sign.py` and `ra8_rot_verify_image()` **in the same
commit**. If they disagree, every verification fails closed -- loudly and
immediately, which is the correct failure direction for a security coupling.

Under the zero-backward-compatibility policy there is no dual-version verifier.
Bump, update both halves, re-sign, delete the old path.

---

## See also

- `ra8_rot.h` -- trailer struct, constants, `ra8_rot_verify_image()`
- `ra8_dfu_antirollback.h` -- the monotonic counter check
- `tools/rot/src/rot_sign.py` -- the signing tool and its `selftest`
- @ref md_docs_2formats_2NSR1 -- the 8-byte NS header that locates this trailer
  at the TrustZone boundary
- @ref md_docs_2formats_2BINARY__FORMATS -- why the magic reads `1TOR` on disk
