<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# tools/cache_bench

A host benchmark that settles two page-cache sizing questions by measurement
rather than argument: which eviction policy and how many frames the cache
should hold (#147, whose decision record is SLRU), and how many *bytes* a frame
-- and therefore a chunk of the `.rabook` container -- should be (#208, feeding
#204). Both sweeps run in CI, so the answers are re-derived rather than
remembered.

The capacity sweep re-models the candidate policies over synthetic and captured
reader traces; a page there is an abstract key, so it can never say anything
about bytes. The block-size sweep is that other axis, and it is what makes the
number evidence rather than opinion: it drives the **real** `ra8_vmem` and
`ra8_vsource` stack with `frame_bytes` set to each swept size, not a model of
them.

## What keeps the comparison honest

- The cache's **byte** budget is held constant across sizes, so a larger block
  means proportionally fewer frames -- the same RAM spend, honestly compared.
- Every byte the cache returns is verified against the source payload and a
  mismatch fails the run, so a fast wrong answer cannot pass as a fast right
  one.
- One leg scans an object sequentially in small reader requests (the miss path,
  where per-request overhead amortises differently at each size); another
  re-reads inside a single block after one cold fill (the pure hit path).
  Splitting them is what lets the per-miss fill cost be *derived* -- subtract
  the independently measured hit cost from the sequential wall time -- instead
  of guessed.
- No trace, payload, container or chunk table is materialized. The only large
  allocation is the resident cache itself; during container setup the
  compressor's state overlays that same region and it is zeroed before
  `ra8_vmem` binds it as measured storage, so the harness never quietly spends
  memory the measurement is supposed to be about.

## Backends: the hardware seam

A backend implements the `cbs_backend_t` seam -- a setup/teardown pair that
publishes an `ra8_vsource_read_fn`, exactly what `ra8_vsource_add_paged`
consumes. Two synthetic host backends ship in-tree: a deterministic payload
source, which measures the harness floor with no container decode, and a real
RBKC chunked `.rabook` with one zlib stream per chunk (the same wrapping
`epub_compile` emits), served through the same bounded header/table/offset
rules and the same inflate path the firmware reader uses. Every miss on that
one pays a genuine inflate of exactly one chunk, so the sweep measures
decompress-cost-per-miss against chunk size -- the number that actually picks
the chunk size -- and not raw byte moves.

SD-over-SPI numbers are a bench follow-up; the seam exists so a backend issuing
real card reads (and later `ra8_cache_store` on OSPI / NAND, #201) plugs in
without touching the sweep core.

## Reading the result

Output is one machine-parseable row per backend, leg and size, followed by
human summary tables and the measured crossover. The durable findings so far:

- **The hit path is flat** across every block size, so block size is a
  miss-side decision only.
- **Sequential throughput climbs steeply and then knees**, and the knee -- the
  smallest block within 90% of peak -- is what names the chunk size. Past it,
  per-miss fill cost keeps growing roughly linearly, so oversizing buys almost
  no throughput and quadruples the page-in latency a reader feels on a cold
  page turn.
- **Small chunks compress markedly worse**, because each zlib stream restarts
  its history; the container itself grows on disk as well.
- **The host knee is a lower bound.** On real storage the per-command cost
  (single-block read loops, #202) dwarfs the host's per-stream setup and pushes
  the knee toward larger blocks, so a hardware run has to confirm before any
  chunk-size *reduction*.
