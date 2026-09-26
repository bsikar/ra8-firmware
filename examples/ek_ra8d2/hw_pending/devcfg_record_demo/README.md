# devcfg_record_demo

First app that links `ra8_devcfg`.

`ra8_devcfg` owns the versioned, CRC-32-protected per-unit record (VCOM,
serials, the `ra8_touch_cal` blob, key identity) behind a two-copy header-last
commit. It reaches its medium only through the injected `ra8_devcfg_store_t`
seam, which made it fully host-testable
(`tests/misc/src/test_ra8_devcfg.c`) and left it with no firmware consumer at
all. This app is that consumer.

## What it proves

1. An app-owned RAM medium pre-filled with `0xFF` stands in for a blank,
   never-programmed window. The blank resolve is checked first:
   `ra8_devcfg_is_blank` reports UNPROVISIONED and `ra8_devcfg_get_vcom_mv`
   refuses rather than handing back a plausible-looking default (INV-VCOM-1).
2. A populated record is committed, re-loaded, and compared field for field,
   which exercises the encode, the CRC, the two-copy resolver, and the VCOM
   validity gate end to end from firmware rather than from a host test.
3. The production extra-MRAM store is then loaded **read-only** to report
   whether this unit is provisioned.

## Why the unit probe never writes

The extra-MRAM window is one-time-programmable on this silicon
(HUM Ch 59.7.4.5): there is no rewritable data flash to erase and re-use, and
each commit consumes a fresh slot. An example that committed to the real store
would burn a provisioning slot on every run, so the write path here is bound to
the RAM medium and the real store is only ever read.

## Run

Console is SCI8 over the J-Link OB VCOM at 115200.

```
just build devcfg_record_demo
just emu devcfg_record_demo
```

Expected banner:

```
devcfg_record_demo: boot
devcfg_record_demo: record round trip PASS
devcfg_record_demo: unit UNPROVISIONED (read-only probe)
```

## Status

`hw_pending`. The RAM leg touches no peripheral beyond the console, so
ra8_emulator runs it as-is, but it has not been captured on the bench.
