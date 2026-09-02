# Application Third-Party Libraries

This directory contains vendored dependencies used by application/content
features and their companion host tools. They are not platform middleware;
first-party facades and ownership seams remain adjacent under
`apps/shared_libs/`.

| Library | Application ownership |
|---|---|
| `libwebp` | Reader image decode, through `apps/shared_libs/webp/` |
| `litehtml` | EPUB reflow/rendering |
| `miniz` | Book/archive compression and decompression |
| `stb` | Reader raster image and font parsing |
| `xz_embedded` | Reader archive decompression |

Vendor bytes are governed by `.gitattributes`, the registry in
`scripts/gen/sbom_registry.py`, the upstream manifests under
`docs/sbom/upstream/`, and the component qualifications under `docs/SOUP/`.

Ownership follows the product domain, not every executable that happens to
compile a dependency. Miniz and the media codecs remain app-owned when a host
content compiler or viewer reuses them, because those tools support the same
reader/content vertical rather than establishing a platform-wide contract.
