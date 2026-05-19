USB CDC Echo Throughput Benchmark Results
==========================================

Hardware:     Renesas EK-RA8D2 (Cortex-M85 @ 1 GHz)
Host:         Raspberry Pi 5 (Linux 6.8, cdc_acm kernel driver)
Firmware:     `tz_secure_only_usb_fs` (FS) and `tz_secure_only_usb_hs` (HS),
              both under `examples/ek_ra8d2/hw_validated/hil/`.
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
Linux `cdc_acm` URB-completion pipeline on the host, not by the
firmware or the wire. See "Why the throughput stops where it does"
below for the full chain-of-causality.

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

Both USBs active simultaneously: status and the path
----------------------------------------------------

Both USB controllers are independent at the silicon level (separate
PHYs, separate ISR lines, separate MSTP gates, no shared clock domain).
The blocker for a single firmware image enumerating both as USB
devices is the **USBX device stack**: USBX's device stack is a
singleton -- `_ux_device_stack_initialize` creates one stack with one
device descriptor and a single set of class interfaces. Two DCD
instances under one stack would land us with one device, one
device-descriptor, but two SETUP-handler paths -- not what the USB
spec expects, and not what USBX is built to do.

**What is already in place for dual operation:**

1. `ra_usb_attach_handler(speed, fn, ctx)` is now per-controller, so
   one firmware image can register *different* upper-layer handlers
   on the two speeds. The HAL routes BRDY/BEMP/CTRT/etc. for FS and
   HS to independent callback slots.
2. The DCD bridge (`port/usbx/ux_dcd_ra_usb.c`) claims its speed's
   slot when initialized; the other slot stays available.
3. PIPECFG.DBLB + PIPEBUF allocation are programmed correctly on
   bulk pipes, so the second controller (when added) gets the same
   firmware-side throughput characteristics as the first.

**What remains: a "bare CDC" handler (~500 LOC) for the second
controller.** This handler would:

* Drive its controller through the same `libs/ra_hal/src/ra_usb.c`
  primitives (`ra_usb_queue_out`, `ra_usb_queue_in`,
  `ra_usb_read_setup_*`, `ra_usb_set_address`,
  `ra_usb_control_response`, etc.) -- no USBX.
* Implement a minimal chapter-9 SETUP state machine
  (`GET_DESCRIPTOR` for device, configuration, string,
  device-qualifier; `SET_ADDRESS`; `SET_CONFIGURATION`;
  `GET_STATUS`; `CLEAR_FEATURE`; `SET_INTERFACE`).
* Implement two CDC class requests (`GET_LINE_CODING` returning a
  fixed line coding; `SET_CONTROL_LINE_STATE` accepting any state).
* Carry its own descriptors (CDC-ACM, distinct VID:PID/strings so
  the host can tell the two devices apart on the same USB bus).
* Auto-echo bulk-OUT to bulk-IN exactly like the bridge does.

The HAL groundwork above is the prerequisite work; the bare-CDC
handler is the standalone next step. Until that lands, the supported
pattern is **one controller per firmware image** and the HIL job
exercises both back-to-back. See "HIL coverage" above.

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

Why the throughput stops where it does
--------------------------------------

The streaming numbers above are stable and the device-side
optimisation work IS done. PIPECFG.DBLB is enabled with a 2*MPS
PIPEBUF region per bulk pipe (HUM Ch 36.2.24, 36.2.25), and
`ra_usb_queue_out` drains banks per the FSP pattern (W0C BRDYSTS at
the top, BCLR only for ZLPs). A first attempt with the wrong drain
order truncated 1 MB streams at exactly 524 288 B because clearing
BRDYSTS after the bank-A drain wiped bank-B's fresh edge -- that's
fixed. Verified via the libusb async bench at
`scripts/usb_libusb_bench.py`: when the host pipelines URBs without
cdc_acm overhead, the firmware echo path can sustain the same rates
as cdc_acm and (with more URBs in flight) higher.

The ceiling we observe (~2.66 MB/s on HS, ~360 KB/s on FS) is the
**Linux cdc_acm URB-completion pipeline**, not the firmware:

* `cdc_acm` posts MPS-sized read URBs (512 B at HS, 64 B at FS) and
  completes each one on a single packet from the device.
* Per-URB completion-callback latency between the kernel re-posting
  the URB and the device having a new IN buffer to fill caps the
  per-microframe packet rate. We measured ~66 % of the 4 MB/s HS
  microframe ceiling.
* Increasing the per-URB byte budget would help, but that requires a
  kernel patch -- it is not something this firmware can fix.

Three workarounds exist if higher rates are needed by a specific
application:

1. **libusb on the host**, async transfers, ring of >= 16 URBs in
   flight. Bypasses cdc_acm completely. Works today against
   unmodified firmware -- see `scripts/usb_libusb_bench.py` for the
   scaffolding.
2. **Patch cdc_acm read URB size** to >= 4 KB. Host-side, Linux
   kernel change.
3. **Vendor-specific bulk-only interface** instead of CDC-ACM, with
   a custom host tool. Loses CDC compatibility (no /dev/ttyACM*
   any more).

None of these are appropriate for the CDC-ACM-echo posture of this
firmware. The matching comment block in
`port/usbx/ux_dcd_ra_usb.c::internal_irq_walk_pipe` and the
queue_in / queue_out headers point readers at this doc.
