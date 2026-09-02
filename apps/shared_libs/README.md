<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# apps/shared_libs/

Reusable application-domain components live here. Each first-party compiled
module keeps production sources in `src/`, public headers in `inc/`, and a
nested `tests/` build unit with its own `src/` and `inc/` directories.

## Third-party ownership

Vendored dependencies used by reader/content features live in
`third_party/<component>`, beside their first-party facades and ownership
seams. Ownership follows the product domain rather than every executable that
compiles the code: Miniz and the media codecs remain app-owned when companion
host compilers, packers, or viewers reuse them. Such a tool is part of the same
content vertical and does not create a platform-wide dependency contract.

A dependency used exclusively by one host tool may instead use a reserved
namespace. The future path `tools/<tool>/third_party/<component>` must not become a
general tools vendor bucket and requires an SBOM registry entry, upstream
manifest, SOUP qualification, license inventory entry, and raw-byte checkout
rule. No current dependency qualifies for tool-private ownership.

Platform and middleware dependencies with genuine cross-domain consumers stay
under `libs/third_party/`.
