<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# `tools/cache_bench` -- page-cache benchmarks (#147 capacity, #208 block size)

A host C program with two independent sweep modes:

| Mode | Axis swept | What it drives | Question it answers |
|------|------------|----------------|---------------------|
| default | cache **capacity** (frames) | re-modelled eviction policies (FIFO / Random / LRU / CLOCK / SLRU / SRRIP) over synthetic + captured reader traces | which eviction policy (#147 decision record: SLRU) and how many frames |
| `--sweep-block` | block / frame / chunk **size in bytes** | the **real** `ra8_vmem` (SLRU) + `ra8_vsource` stack, with `frame_bytes` set to the swept size | what chunk size the chunked `.rabook` container / `ra8_vmem` `frame_bytes` should use (#208, feeding #204) |

Build and run:

```sh
make -C tools/cache_bench            # build
make -C tools/cache_bench run        # capacity sweep (the #147 decision record)
make -C tools/cache_bench sweep      # block-size sweep (#208)
tools/cache_bench/cache_bench <name>=<path>   # replay a captured trace
                                              # (one "<object> <page>" per line)
tools/cache_bench/cache_bench --output=report.md # atomic sibling publication
```

Both modes also run in CI via `make bench-cache`.

## `--sweep-block`: the #208 block/frame-size sweep

The capacity sweep treats a page as an abstract key; it can never say how many
**bytes** a frame should be. This mode sweeps that axis -- 512 B, 1 K, 4 K,
16 K, 64 K, 256 K -- with the methodology of Memory Systems Ch 23.2.1/23.2.2:

- **leg a (`seq`)** -- a sequential whole-object scan in 256 B reader
  requests. Small blocks pay per-request overhead on every one of many
  backend commands; large blocks amortise it. The **knee** -- the smallest
  block within 90% of peak sequential throughput -- names the chunk size.
- **leg b (`hot`)** -- repeated reads inside one block: one cold fill, then
  pure cache hits. This isolates the hit path and shows it is independent of
  the block size (so a larger block costs nothing on hits -- only on misses).

The cache **byte budget is held constant** (1 MiB) across sizes, so a bigger
block means fewer frames: the same RAM spend, honestly compared. Every byte
the cache returns is verified against the source payload; a mismatch fails
the run.

### Backends (the hardware seam)

Backends implement the `cbs_backend_t` seam in `sweep_block_internal.h`
(implementations live in `sweep_block_backends.c`) -- a `setup`/`teardown`
pair that publishes an `ra8_vsource_read_fn`, exactly what
`ra8_vsource_add_paged` consumes. Two synthetic host backends ship in-tree:

- **`mem`** -- the checkpointed deterministic payload source directly. The
  harness floor includes bounded payload generation but no container decode.
- **`rbkc-z9`** -- a real "RBKC" chunked `.rabook` container streamed to a
  host-composed scratch transaction
  with one zlib level-9 stream per chunk (the same wrapping
  `tools/epub_compile` emits, `chunk_bytes` = the swept size), served by the
  same bounded header/table/offset rules and low-level tinfl stream used by
  the firmware reader. Every miss pays genuine scratch reads plus a tinfl
  inflate of exactly one chunk, so the sweep measures
  **decompress-per-miss cost per chunk size** -- the number that actually
  picks the `.rabook` chunk size -- not just raw byte moves. The `src MiB`
column doubles as a compression-ratio readout: small chunks compress
markedly worse (each stream restarts its zlib history).

No trace, payload, container, or chunk table is materialized. The sole large
composition backing is the benchmark's intentional aligned 1 MiB resident
cache. During RBKC setup only, caller-owned tdefl state overlays that region;
compression finishes and the full region is zeroed before `ra8_vmem` binds it
as measured cache storage. A target may place the same semantic backing in
external RAM. All metadata exposes exact required/supplied workspace bytes.

**Hardware leg (follow-up):** SD-over-SPI numbers are a bench follow-up --
the tool deliberately takes its backing through the `cbs_backend_t` seam so a
third backend that issues real card reads (and, later, `ra8_cache_store` on
OSPI/NAND, #201) plugs in without touching the sweep core. On hardware the
per-command cost (CMD17 single-block loops, #202) is far larger than the
host's per-stream setup, which pushes the knee toward larger blocks -- so the
host knee is a lower bound, and the hardware run must confirm before any
chunk-size reduction.

### Output

One machine-parseable row per (backend, leg, size):

```
sweep-block backend=rbkc-z9 leg=seq block=65536 frames=16 reads=131072 hits=130560 misses=512 evictions=496 backend_calls=512 backend_bytes=33554432 src_bytes=6505836 backing_bytes=1627515 wall_us=83436 mib_s=383.5 ns_per_read=636.6
```

followed by human summary tables and the measured crossover. `backend_calls`
counts storage commands (one per miss), `backend_bytes` the bytes delivered
to the cache (inflated), `src_bytes` the raw medium traffic (compressed
stream bytes for `rbkc-z9`), and `est us/miss` the per-miss fill cost derived
by subtracting the independently measured hit-path cost (leg b) from the
sequential wall time.

### Sample run (Apple M-series host, `cc` -O2)

```
## Summary (payload 8 MiB, cache budget 1 MiB, 256 B reader requests)

### `rbkc-z9` -- sequential whole-object scan (leg a)

| block | frames | MiB/s | ns/read | est us/miss | backend calls | src MiB | backing MiB |
|------:|-------:|------:|--------:|------------:|--------------:|--------:|------------:|
|  512B |   2048 |   109 |  2238.8 |         4.4 |         65536 |   14.08 |        3.64 |
|    1K |   1024 |   165 |  1481.0 |         5.9 |         32768 |   11.37 |        2.91 |
|    4K |    256 |   304 |   803.6 |        12.6 |          8192 |    8.60 |        2.17 |
|   16K |     64 |   405 |   603.3 |        37.7 |          2048 |    7.11 |        1.78 |
|   64K |     16 |   481 |   507.4 |       125.2 |           512 |    6.20 |        1.55 |
|  256K |      4 |   498 |   489.8 |       482.5 |           128 |    5.85 |        1.46 |

### `rbkc-z9` -- same-block re-read (leg b, pure hit path)

| block | ns/read | hits | misses |
|------:|--------:|-----:|-------:|
|  512B |    15.8 | 1048575 |      1 |
|    1K |    14.9 | 1048575 |      1 |
|    4K |    17.3 | 1048575 |      1 |
|   16K |    13.5 | 1048575 |      1 |
|   64K |    18.2 | 1048575 |      1 |
|  256K |    18.6 | 1048575 |      1 |

### Measured crossover (rbkc-z9, sequential)

Peak 498 MiB/s at 256K; knee (first size within 90% of peak) at 64K
(481 MiB/s, 96.5% of peak).
The measured knee lands on the current 64K `.rabook` chunk default (#204):
keep it.
```

Reading the measurement:

- **Sequential throughput climbs steeply to 64 K then flattens** (481 -> 498
  MiB/s buys 3.5% for 4x the block): per-stream overhead has stopped
  dominating by 64 K. The knee confirms the 64 KiB default from #204.
- **The hit path is flat** (~14-19 ns/read at every size): block size is a
  miss-side decision only.
- **Per-miss fill cost grows linearly past the knee** (125 us at 64 K vs
  483 us at 256 K): going above the knee buys almost no throughput and
  quadruples the page-in latency a reader feels on a cold page turn.
- **Small chunks also compress worse**: 512 B chunks move 14.08 MiB of
  compressed data where 64 K chunks move 6.20 MiB for the same payload
  (and the container itself is 2.3x larger on disk).

The `mem` backend rows (see a live run) provide the same tables for a free
backend, separating harness cost from decompress cost.
