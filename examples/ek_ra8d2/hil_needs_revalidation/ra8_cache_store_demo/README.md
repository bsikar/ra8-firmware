# ra8_cache_store_demo

Runs the whole `ra8_cache_store` lifecycle end to end on the target -- the
persistent key-to-blob cache (#201) that backs compiled `.rabook` containers,
which had no direct example until #257.

One pass covers: format and mount a store over LevelX standalone; put and get
several keyed render/glyph blobs whose sizes cross sector boundaries and read
each back byte-identical; pin the cover atlas so it can never be evicted, evict
a render tile, and confirm the evicted key is now a miss, the survivors still
resolve, and evicting the pinned atlas is refused as busy; re-put a new blob to
prove the evicted entry's sectors were reclaimed; then checkpoint-close and
re-mount over the same media with a fresh zeroed control block, confirming the
survivors come back byte-identical, the evicted key stays gone, and the pin
survived the checkpoint.

## Why a RAM-backed NOR driver

The store's physical-flash bind is an injected callback
(`ra8_cache_store_nor_init_fn`). Production binds the Octo-SPI driver; this demo
binds a RAM driver, so the entire persistent-cache path runs in SRAM with no
MMIO. That buys two things: it needs no external hardware, and because the path
touches no peripheral register there is nothing for an emulator to model
differently from the chip -- an emulated run executes byte-identical
instructions to the on-silicon one.

The RAM backing persists across close and re-init within one boot, which models
the property the persistence check needs: control RAM lost, on-media content
survives. A real Octo-SPI part persists across an actual power cycle; the RAM
model persists across the in-process remount.

The app forces `RA8_USE_LEVELX_STANDALONE=ON`, which builds the vendored LevelX
NOR sources with `LX_STANDALONE_ENABLE` so no ThreadX is pulled in.

The same core and the same RAM NOR driver are compiled into the host unit test,
so `make test` drives byte-identical logic on x86_64.
