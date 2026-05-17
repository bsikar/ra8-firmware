USB CDC Echo Throughput Benchmark Results
==========================================

Hardware:     Renesas EK-RA8D2 (Cortex-M85 @ 1 GHz)
Host:         Raspberry Pi 5 (Linux 6.8, cdc_acm kernel driver)
Firmware:     `tz_secure_only_usb` (FS) and `tz_secure_only_usb_hs` (HS),
              both under `examples/ek_ra8d2/hw_validated/smoke/`.
Connector:    USBFS on J11 (micro-USB, hub port 4),
              USBHS on J7  (USB-C,    hub port 1).

Two benchmark methodologies are used. They measure different things and
the difference between them is informative:

1. **Chunked round-trip** (`scripts/usb_benchmark.py`).
   Host writes a chunk, waits for the entire chunk to echo back, verifies
   bit-exact, then writes the next. Single-buffered. Useful for *latency*
   characterisation but its measured throughput is dominated by the
   per-chunk polling round-trip, not USB line rate.

2. **Pipelined streaming** (`scripts/usb_stream_bench.py`).
   A writer thread fires the entire payload back-to-back; the main
   thread drains in a tight non-blocking `select()` loop. Measures the
   sustained wire throughput the firmware + host driver achieve when
   neither side waits on the other. This is the right number for
   "max data rate".

Streaming throughput (pipelined, no waits)
------------------------------------------

USBHS (J7, 480 Mbps line rate, MPS=512):
  1 MB stream    : ~2.66 MB/s one-way    (~5.32 MB/s aggregate, ~21.7 Mbps)

USBFS (J11, 12 Mbps line rate, MPS=64):
  64 KB stream   : ~360 KB/s one-way     (~720 KB/s aggregate, ~2.95 Mbps)

These are stable across runs (variance under 5%) and limited by the
firmware's single-buffer auto-echo path, not by the host or the wire.
See "Future work" below for the optimisation path that exists on paper
but is not yet shipped.

Chunked round-trip (per-chunk gated, single-buffered)
-----------------------------------------------------

USBHS at MPS chunk (512 B):  ~460 KB/s one-way (~920 KB/s aggregate)
USBFS at MPS chunk (64 B) :  ~58  KB/s one-way (~115 KB/s aggregate)

The chunked numbers are about 5-6x lower than the streaming numbers
because each chunk pays a ~1 ms host-side poll-and-verify latency.
These remain useful as a strict correctness gate: any data shuffle or
short read is caught on the first wrong chunk.

Correctness
-----------

  USBHS : 64/64 lengths 1..64 echo perfectly,
          50/50 random 1..255 B payloads echo perfectly,
          1 MB streaming round-trip verifies bit-exact (no mismatches).
  USBFS : same coverage, 64 KB streaming, bit-exact.

Enumeration
-----------

  USBHS : reliable on the first Tapo power-cycle attempt every time.
          MPS=512 bulk endpoints reported correctly.
  USBFS : reliable on the first power-cycle when the board has been
          unpowered for >= 5 s; needs occasional retry when transitioning
          from a freshly-flashed J-Link image. Wired retry loop in
          `scripts/hil_usb_test.sh` (5 tries, Tapo cycle each).

PPPS re-enumeration
-------------------

  USBHS : hub-level PPPS (`uhubctl -l 2-1.3 -p 1 -a cycle`) is reliable.
          5/5 cycles OK.
  USBFS : hub-level PPPS does *not* propagate cleanly through the FS PHY
          (the device-side D+ pull-up stays asserted across the hub-side
          toggle, host xhci-hcd never sees a fresh attach). Use the
          host-side `authorized` sysfs toggle (`scripts/hil_ppps.sh
          --soft cycle 4`); 5/5 cycles OK. Both paths force the kernel
          to re-run chapter-9 enumeration without dropping board power.

HIL coverage (CI)
-----------------

`scripts/hil_usb_test.sh` is the canonical end-to-end test and runs in
the `hil-usb-suite` job of `.github/workflows/hil.yml`. For each
controller in turn:

  1. Flash via J-Link (`hil_flash.sh`).
  2. Tapo power-cycle the board.
  3. Wait up to 35 s for enumeration; retry up to 5 times.
  4. Run `usb_benchmark.py` for correctness (1..64 lengths + 50 random
     payloads + chunked round-trip).
  5. Run `usb_stream_bench.py` and assert the one-way throughput is
     above the floor (USBFS: 250 KB/s, USBHS: 2000 KB/s).
  6. Re-enumerate via PPPS (hard for HS, --soft for FS).

Both controllers are exercised in the same job; the suite passes only
when BOTH succeed.

Why not "both USBs active simultaneously in one firmware?"
---------------------------------------------------------

Both USB controllers are independent at the silicon level (separate
PHYs, separate ISR lines, separate MSTP gates, no shared clock domain).
What blocks a single firmware from running them concurrently is the
**USBX device stack** layer above our DCD bridge: USBX's device stack
is a singleton — `_ux_device_stack_initialize` creates one stack with
one device descriptor and a single set of class interfaces. Wiring two
DCD instances under one stack would land us with one device, one
device-descriptor, but two SETUP-handler paths — not what the USB spec
expects.

The DCD bridge itself (`port/usbx/ux_dcd_ra_usb.c`) uses a single
`s_dcd` static. Refactoring it to be per-controller is mechanical, but
without USBX support upstream it would not get us a working "dual
device" — the host would still see one CDC interface, gated by whichever
controller registered first.

Until USBX is replaced or substantially rewritten, the supported pattern
is one controller per firmware image. The HIL job exercises both back-
to-back, which gives the same end-to-end coverage as a "concurrent"
test would.

Implementation notes (echo path)
--------------------------------

CDC echo is implemented via the bridge's ISR-side auto-echo
(`ux_dcd_ra_usb_auto_echo_enable` in `port/usbx/ux_dcd_ra_usb.c`).
When BRDY fires on the configured bulk-OUT pipe, `internal_irq_walk_pipe`
drains the packet and immediately re-queues it on the bulk-IN pipe --
no thread-mode dispatch and no `_ux_device_class_cdc_acm_read/_write`
round-trip. This runs entirely inside the dispatch handler.

The proper USBX worker-thread path also works for individual packets
(the demo's worker thread is dispatched by PendSV correctly now that
the RA8D2 USBHS USBR-driven IRQ storm is gated out by the strict ISR
gate in ux_dcd_ra_usb.c and the BEMPSTS clear in the IN-pipe completion
path). Auto-echo just shortcuts the per-transfer thread round-trip.

Future work (not in this commit)
--------------------------------

The streaming numbers above are about 8-10% of the wire's bulk
ceiling. The bottleneck is the single-buffer auto-echo: each ISR
processes one packet (drain OUT, queue IN, return). To get past the
"one packet per IRQ" ceiling we need two things in concert:

1. **Hardware double-buffering**. Set `PIPECFG.DBLB` on bulk pipes
   and program `PIPEBUF` with a 2*MPS region per pipe (HUM Ch 36.2.24,
   36.2.25). The scaffolding for this is in
   `libs/ra_hal/inc/ra8d2_usb_regs.h` (PIPEBUF field defs added) and
   `libs/ra_hal/src/ra_usb.c` (helper `internal_pipebuf_word()` is
   present; activation is commented `TODO` until point 2 is done).
2. **Bank-aware drain in the queue_out/queue_in HAL paths**. With
   DBLB on, BRDY fires per-bank; if `queue_out` doesn't drain both
   banks the second bank stalls and throughput halves rather than
   doubles. A first attempt confirmed this empirically (1 MB stream
   on HS truncated at exactly 524 288 B, i.e. half).

Once both are in place the expected gains are ~2-3x on HS and ~1.5-2x
on FS. The ZLP cadence may also need tuning (the host's read URB is
MPS-sized so a per-packet ZLP doubles IN transactions for free; a
per-N-packet ZLP works on HS but breaks the FS host driver's URB
completion path -- so leave it per-packet for now).
