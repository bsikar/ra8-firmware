# cache_store_paged_demo

Maintained consumer for `libs/ra8_cache_store` (issue #937): the persistent
key(CRC-32) -> blob cache had no application driving it on `dev`. Its only
caller lived under `examples/ek_ra8d2/hil_needs_revalidation/`, and
`threadx_levelx_demo` links the lib without ever calling it.

This app deliberately covers the parts of the public surface that parked demo
never touched, so it is a complement rather than a copy.

## What it drives

| Leg | What it proves |
| --- | --- |
| `mount` | Format + mount over a RAM-backed LevelX NOR driver the app owns. |
| `seed` | `put` of a 1300-byte blob (3 payload sectors + a partial tail) and a sub-sector blob, then `get`. |
| `paged` | `ra8_cache_store_read` as the `ra8_vsource_read_fn`-shaped reader: whole blob, head window, two windows straddling a 512-byte logical-sector boundary, the partial tail, plus the `k_ra8_err_out_of_range` and NULL guards. Every window is byte-compared against a position-dependent pattern, so a read that lands the wrong offset fails. |
| `replay` | `sync` (directory checkpointed, clean marker left unset), one more `put` *after* the checkpoint, then a simulated power loss: a fresh `LX_NOR_FLASH` control block is mounted over the same media with `format=false` and **no** `close`. Mount must take the dirty path, replay the append log, and bring back all three keys, including the one the checkpoint never saw. |
| `guards` | Write-once refusal (`k_ra8_err_exists`), zero-length and NULL puts, not-found lookups, and the pin/evict interlock (`k_ra8_err_busy` until unpinned). |
| `unwind` | `close`, then every operation on the closed handle reporting `k_ra8_err_not_initialized`, plus the three `ra8_cache_store_init` argument rejections. |

## Why a RAM-backed NOR driver

The store's physical-flash bind is an injected callback. Production binds the
Octo-SPI driver; this app binds the RAM driver in `src/main.c`, so the whole
path runs in SRAM with no MMIO and an emulated run is byte-identical to an
on-silicon run. The backing array survives the simulated power loss, which is
the "control state lost, media survives" model the `replay` leg needs.

## Output

Success (the only success banner, emitted once and then re-emitted every second
so a scrape sees the steady state):

```
[csp] cache_store paged demo PASS bytes=<n> recovered=3 guards=<n>
```

Failure:

```
[csp] cache_store paged demo FAIL stage=<1..6> status=<ra8_err_t>
```

Stages are `1 mount`, `2 seed`, `3 paged`, `4 replay`, `5 guards`, `6 unwind`.

## Build

```sh
cmake --preset ra8d2-debug
ninja -C cmake-build-debug cache_store_paged_demo.elf
```

## Status

`hw_pending`: it compiles for the target and is self-checking, but it has not
been run on the bench yet, so it carries no `hil.conf`.
