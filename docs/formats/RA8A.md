# RA8A -- The `.ra8app` Signed Module Container

**Magic:** `RA8A` (`0x52413841`) &nbsp;|&nbsp;
**Library:** `libs/ra8_app` &nbsp;|&nbsp;
**Producer:** `ra8app_pack` (`libs/ra8_app/tools/ra8app_pack.zig`) &nbsp;|&nbsp;
**Size:** 160-byte header, then the module image

---

## 1. Synopsis

A third-party app reaches the RA8D2 as a `.ra8app` file: a fixed 160-byte
header followed by a ThreadX module image, the bytes the Module Manager loads.
Before any of the image is trusted, the loader has to settle three things, and
the header holds the answer to each:

1. **Is this one of ours, and can this firmware run it?** The magic, the
   format revision, and `min_api_version` against the call table the firmware
   serves (see [the module call table](../APP_API.md)).
2. **What may it reach?** A capability bitfield the app declares and the host
   grants or refuses.
3. **Who built it?** An Ed25519 signature over the header and the payload,
   checked against a pinned public key.

If you stop reading here: *a `.ra8app` is a 160-byte little-endian header
(identity, sizes, API floor, capability manifest, Ed25519 signature) in front
of a ThreadX module binary; the loader refuses it unless the header parses,
the host grants every declared capability, and the signature verifies, in that
order.*

---

## 2. Design rationale

### Why a container and not the module binary alone

A ThreadX module binary already starts with a preamble (`txm_module_preamble.S`)
that names its entry point, stack and sizes. It says nothing about who built it
or what it may touch, and nothing in it is signed. The container adds exactly
those, and copies the two preamble facts the loader must bound before it reads
the module (`entry_offset`, `stack_size`) into a header it can check first.

### Why the signature sits in the middle

The signed message is the header up to the signature field, then the payload:
two runs that are not contiguous in the file. Because the signature is part of
the fixed-size header, every field the loader reads before it trusts the image
(sizes, API floor, capabilities, identity) sits in the signed prefix, and that
prefix is the same `0x60` bytes in every file. The signer and the verifier
share one definition of the two runs (`appimg_verify.signedMessage`), so they
cannot disagree about which bytes are covered.

### Why capabilities are declared rather than granted

The manifest is a request. The host compares it with what it is willing to
give and refuses the load when the app asks for more. An undefined capability
bit makes the header invalid instead of being ignored, so a newer app cannot
quietly lose a permission on older firmware and run half-working.

### Why an unsigned image is refused before the backend is asked

An unsigned image has an all-zero signature field (the packer's state before
it signs). The gate refuses that pattern itself, so "unsigned modules are
rejected" holds even against a verify backend that would accept a zero
signature.

---

## 3. Wire format

Little-endian throughout. Every field is naturally aligned and the header has
no padding, so the first 160 bytes of the file *are* the parsed header
(`appimg.Header`, `ra8_appimg_header_t`).

| Offset | Size | Field | Rule |
|-------:|-----:|-------|------|
| `0x00` | 4 | `magic` | `0x52413841` |
| `0x04` | 4 | `format_version` | `1` |
| `0x08` | 4 | `entry_offset` | `< code_size`; byte offset of the entry into the code |
| `0x0C` | 4 | `code_size` | `1 ..= 0x100000` |
| `0x10` | 4 | `data_size` | `0 ..= 0x100000` |
| `0x14` | 4 | `stack_size` | `0x400 ..= 0x100000` |
| `0x18` | 4 | `min_api_version` | `<=` the firmware's `api_version_current` (1) |
| `0x1C` | 4 | `capabilities` | only bits in `0x7` |
| `0x20` | 32 | `app_id` | NUL-terminated inside the field, not empty, zero padded |
| `0x40` | 32 | `display_name` | NUL-terminated inside the field, zero padded |
| `0x60` | 64 | `signature` | Ed25519 `R || S`, not all zero |
| `0xA0` | `code_size` | code | the module binary |
| `0xA0 + code_size` | `data_size` | data | initial data, if any |

Capability bits:

| Bit | Name | Grants |
|----:|------|--------|
| `0x1` | `display` | drawing in its screen region, and its input |
| `0x2` | `storage` | files under `/apps/<app_id>/` |
| `0x4` | `network` | network connections (no call uses it yet) |

The signed message is `file[0x00 .. 0x60] || file[0xA0 .. 0xA0 + code_size + data_size]`.
Bytes past the declared payload are not signed and not loaded.

---

## 4. Algorithms

### Producing (`ra8app_pack`)

```
ra8app_pack SEED_FILE APP_ID DISPLAY_NAME CAPABILITIES MODULE.bin OUT.ra8app
```

1. Read the module preamble (`txm_preamble.read`): the preamble ID (`MODU`),
   the thread shell entry, the start stack size, and the code size, which must
   equal the binary's length.
2. Build the header (`appimg_pack.header`): the preamble's entry and stack,
   `code_size` = the whole binary, `data_size` = 0 (a module's data is built in
   RAM at load, not carried in the file), `min_api_version` = the current
   generation, and the caller's identity and capabilities.
3. Lay out `header || code || data` with a zero signature, and prove it with
   `appimg.parse`, so the packer cannot emit a file the parser refuses.
4. Sign the two runs with RFC 8032 Ed25519 and write the 64 bytes at `0x60`.
   Signing is deterministic: the same input and seed give the same file.

### Admitting (`appimg_verify.verify`, `ra8_appimg_verify`)

The order is the policy:

1. `appimg.parse`: length, magic, revision, capability bits, text fields, API
   floor, then sizes against their caps and against the file length.
2. The host's grant against the manifest. A grant with an undefined bit is
   itself malformed.
3. Refuse an all-zero signature.
4. Ask the platform's Ed25519 backend. Only `good` admits.

---

## 5. Memory behaviour

Parsing copies the 160-byte header onto the caller's stack and reads nothing
past it except to compare the file length. Verifying adds no allocation: the
signed message is two slices into the caller's buffer. The packer, which runs
on the build machine only, allocates the file and one scratch copy of the
signed message.

---

## 6. Worked example

`com.example.notes`, display name `Notes`, capabilities `display | storage`,
packed by `ra8app_pack` with the public test seed (`0x24` x 32, see
`libs/ra8_app/tools/test_key.zig`) from a 64-byte module whose preamble puts
the entry at `0x30` and asks for a 1 KiB stack:

```
00000000: 41384152 01000000 30000000 40000000   magic, format 1, entry 0x30, code 0x40
00000010: 00000000 00040000 01000000 03000000   data 0, stack 0x400, min_api 1, caps 0x3
00000020: 636f6d2e 6578616d 706c652e 6e6f7465   app_id "com.example.note
00000030: 73000000 00000000 00000000 00000000            s" + NUL padding
00000040: 4e6f7465 73000000 00000000 00000000   display_name "Notes"
00000050: 00000000 00000000 00000000 00000000
00000060: 864865d3 4cd3645c 1a2baef1 6770b428   signature (64 bytes) ...
00000070: 7b231c3c 92acaca3 733faaff 15d41942
00000080: 3e520f68 909c31c0 962b608e 77ddb3b8
00000090: 8a2662b8 cab74ebb 72fea939 50f9130a   ... to 0x9f
000000a0: 55444f4d 00000000 00000000 00000000   payload: preamble ID "MODU"
```

Note the magic's bytes on disk are `41 38 41 52` ("A8AR"): `0x52413841` read
most significant byte first is "RA8A". The signed message here is
`0x00..0x60` followed by `0xA0..0xE0`.

---

## 7. Failure modes

Every refusal is default-deny: the caller gets a non-`k_ra8_ok` code and must
not load anything.

| Refusal | `ra8_err_t` | Cause |
|---------|-------------|-------|
| `ShortImage` | `invalid_size` | file shorter than the header, or than the payload it declares |
| `Validation` | `validation_failed` | bad magic or revision, undefined capability bit, unterminated or empty `app_id`, unterminated `display_name`, all-zero signature, or a grant with an undefined bit |
| `Unsupported` | `not_supported` | `min_api_version` above this firmware, or the platform has no Ed25519 |
| `OutOfRange` | `out_of_range` | a size outside its cap, a stack under 1 KiB, or `entry_offset >= code_size` |
| `AccessDenied` | `access_denied` | the app declares a capability the host withheld |
| `BadSignature` | `crc_mismatch` | the signature did not verify |

A failed signature deliberately reports the same code whatever the backend
said, so an attacker learns only that the image did not authenticate.

Things the header cannot do: a declared size cannot make the loader read past
the file (sizes are checked against its length first), and a forged capability
cannot be added after signing (the manifest is inside the signed prefix).

---

## 8. Versioning

- `format_version` must be exactly `1`. A new header layout gets a new number,
  and this reader refuses it.
- `min_api_version` is the lowest generation of the module call table the app
  needs. The firmware serves `api_version_current`; request numbers are
  append-only, so an app built for generation N runs on any firmware at N or
  later.
- A new capability bit widens `Capability.known`. Older firmware refuses a
  header that sets it, rather than ignoring it.
