# Real-world EPUB test fixtures + probe

Drop real `.epub` files into `real/` and probe them against our `ra_epub`
pipeline (parse / spine / TOC / cover / chapter-inflate) on the host. This
is how we catch real-world EPUB compatibility gaps that the synthetic
baked fixtures (`tests/test_ra_epub_*.c`) don't.

## Copyright

`real/*.epub` is **git-ignored** (see the repo `.gitignore`) -- the books
are copyrighted and used for **local testing only**; they are never
committed or published. Copy your own EPUBs in:

```
cp ~/Books/some_book.epub tests/fixtures/epub/real/
```

If a redistributable fixture is ever needed in the repo, use a
public-domain EPUB (e.g. Project Gutenberg), not a copyrighted book.

## Probe

`epub_probe.c` opens an `.epub` through `ra_epub_open()` (host /
`RA_SIMULATOR_MODE`, malloc-backed) and prints what our pipeline
extracts. Build + run via:

```
./tests/fixtures/epub/run_probe.sh tests/fixtures/epub/real/your_book.epub
```

## Findings so far (2026-06-20)

Probing two real Boox-sideloaded books surfaced two genuine `ra_epub`
bugs (tracked in the issue tracker):

| Book | open | chapters | NCX TOC | cover |
|------|------|----------|---------|-------|
| 356 KB (32-chapter) | OK | 32/64 | **0 entries parsed (NCX has 77 navPoints)** | none declared in OPF (correct) |
| 7 MB (novel + images) | **FAILS `no_mem`** | - | - | - |

1. **Large EPUBs fail to open** -- the static miniz arena is 96 KiB
   (`k_ra_epub_miniz_pool_bytes`); a 7 MB archive's central-directory /
   decompressor allocations exceed it -> `k_ra_err_no_mem`.
2. **NCX TOC parses zero entries** -- a valid `OEBPS/toc.ncx` with 77
   `<navPoint>`s yields `toc_count = 0` (the NCX is detected, `toc_kind = 1`,
   but the nav entries are not extracted).
3. Cover resolution is **correct** (returns empty only when the OPF truly
   declares no cover) -- still untested on a real cover-bearing EPUB
   (the 7 MB book that has one does not open yet, per #1).
