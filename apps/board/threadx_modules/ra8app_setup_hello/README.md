<!-- Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT -->

# ra8app_setup_hello

The `.ra8app` setup example (RA8FW-1224): the whole module-packaging path,
from a ThreadX module binary to a signed, permission-carrying `.ra8app` the
Module Manager admits or refuses at load, in one readable file.
[`setup_example.zig`](setup_example.zig) runs it under `zig build test`.

The four steps:

1. **The module preamble.** A module binary opens with 16 words the packer
   reads: the shell entry (word 6, Thumb bit set, counted from the word
   itself), the start stack (word 10), and the code size (word 14), which
   must be the whole file because a module's data is RAM the manager
   allocates at load, never bytes in the file.
2. **The identity.** The caller supplies what the binary cannot say about
   itself: `app_id`, `display_name`, and the capability set. Capabilities
   (`display`, `storage`, `network`) are the permissions the host grants or
   withholds; the example asks for display and storage, no network, the
   grant an offline app takes.
3. **Pack and sign.** `module_pack.packModule` writes the fixed header
   (entry, stack, segments, capabilities), appends the module image and
   signs the whole file with Ed25519. The command-line form is the
   `ra8app_pack` tool (`libs/ra8_app/tools/ra8app_pack.zig`): seed file,
   app id, display name, capabilities, module binary, output. The test key
   (`libs/ra8_app/tools/test_key.zig`) is public on purpose: it proves the
   path, never provenance.
4. **The gate.** `gate.verify` parses the header, checks the signature over
   the signed span and applies the host's grant. A capability the app
   declared but the host withheld is a refusal, not a silent downgrade; so
   is a changed byte or a missing tail.

The build graph's wiring for real modules lives in
`tests/zig_build_graph/txm_ra8app.zig` (`add`, `addCheck`) and its verify
test, `txm_ra8app_check.zig`, runs against the `arm/txm_hello_m33.ra8app`
the build installs.
