# libs/ra8_ftl -- ra8_ftl vs LevelX

Two wear-levelling layers over erase-before-write flash, split by **which
medium** and by **who wrote them**.

`ra8_ftl` is first-party and serves the **on-chip** extra MRAM: it wraps an
erase-before-write `ra8_io_blockdev_t` and presents a free-overwrite one upward,
so `ra8_fs` can rewrite a FAT entry in place. LevelX is vendored SOUP and serves
the **external Octo-SPI NOR** (`lx_nor_flash_open()`), reached through the
`ra8_fs` block-device backend `lx_fs_backend_bind()` in `port/levelx/` (or
driven raw, as `libs/ra8_cache_store/` does).

| | `ra8_ftl` | LevelX |
|---|---|---|
| Medium | on-chip MRAM / data flash | external MX25LM512 Octo-SPI NOR |
| Provenance | first-party, gated like the rest of `libs/` | vendored SOUP, exempt (`docs/SOUP/levelx.md`) |
| Sits under | `ra8_io` / `ra8_fs` | `ra8_fs` (via `port/levelx/`) or raw (`ra8_cache_store`) |
| Needs ThreadX | no | yes, unless built via `RA8_USE_LEVELX_STANDALONE` |

**Which do I use?** It follows from the medium, not from preference: on-chip
MRAM -> `ra8_ftl`; the Octo-SPI part -> LevelX. Neither stacks on the other.

<!-- disambig
this: libs/ra8_ftl
that: libs/third_party/levelx
symbol: ra8_ftl_init
symbol: ra8_io_blockdev_t
symbol: lx_nor_flash_open
users: ra8_ftl = 1
users: levelx = 3
-->
