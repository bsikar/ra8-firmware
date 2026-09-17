# ADR-0015: Per-panel tone-curve calibration record and bench procedure

## Status

Proposed.

This records the storage and measurement half of issue #479. The curve
format, the nominal curve and the renderer that consumes them are draft
pull request #1326; this record does not restate them.

## Context

### What exists after the format slice

`libs/ra8_gfx/inc/ra8_gfx_tone.h` (on branch
`ereader/479-panel-gray-level-lut`, draft pull request #1326) publishes two
objects and three entry points:

* `ra8_gfx_tone_lut_t` is the curve: 16 bytes, `level_gray8[n]` being the
  gray8 tone that panel level `n` renders. The contract enforced by
  `ra8_gfx_tone_lut_validate` is strictly increasing knots with
  `level_gray8[0] == 0` and `level_gray8[15] == 255`.
* `ra8_gfx_tone_map_t` is that curve prepared for the hot loop, 768 bytes
  of caller-owned plain data, written by `ra8_gfx_tone_prepare` and read by
  `ra8_gfx_tone_quantise`.
* `k_ra8_gfx_tone_lut_nominal` is the committed even palette (level `n`
  renders `n * 17`), which reproduces the closed-form rule in
  `libs/ra8_gfx/src/ra8_gfx_dither.c` exactly.

So the renderer can consume a measured curve. Nothing can supply one. A
repository code search for `ra8_gfx_tone`, `tone_lut` and `tone_curve`
returns no hits at all, so there is no tone storage, no provisioning path
and no bench artefact anywhere in the indexed tree.

### The precedent this record is modelled on

`libs/ra8_epd_cal/inc/ra8_epd_cal.h` already solves the shape of this
problem for VCOM, and states its reasoning in the file:

* per-device bytes cannot live inside a DFU-signed image, because one
  signature authenticates one byte sequence, so the record carries its own
  CRC-32 and schema version;
* the record must live outside both code banks, since slot A and slot B are
  erased wholesale by an update and reverted by a rollback;
* the store is an injected seam (`ra8_epd_cal_store_t`), so production
  reads flash and host tests read a RAM fixture;
* resolution walks sources in descending authority and reports which one
  answered (`ra8_epd_cal_source_t`).

All four apply unchanged to a tone curve. One thing does not.

### The difference that decides the policy

`libs/ra8_epd_cal/inc/ra8_epd_cal.h` refuses to drive the panel when no
trusted VCOM resolves, because the wrong VCOM leaves a net DC bias across
the film and the damage is cumulative and unrecoverable.

A wrong tone curve has none of that character. The worst case is banding,
a flat-looking page, or midtones that sit wrong; the page is still legible,
nothing degrades, and a rewrite of the record fixes it. Copying the
fail-closed rule from VCOM would mean refusing to render a book because a
cosmetic refinement is missing, which is the wrong trade in the opposite
direction.

### The storage constraint, read off the VCOM record

`libs/ra8_epd_cal/inc/ra8_epd_cal.h` places its 32-byte blob in the
extra-MRAM option-setting window at
`k_ra8_flash_extra_start + k_ra8_epd_cal_extra_mram_offset` (`0x200`), and
carries its own note that the window is one-time-programmable
option-setting memory rather than a rewritable data flash, with a
rewritable home tracked as a bench question by issue #315. It also records
that `0x40 .. 0x1BF` is reserved for the two sequence-numbered copies of
the general per-device configuration record.

A VCOM value is provisioned about once per panel. A tone curve is not: it
is re-measured whenever the panel is replaced, and possibly re-measured
over the life of one panel. A write-once medium is therefore the wrong
home for it, and that is a constraint rather than a preference.

## Decision

1. **The measured curve is per-device data and takes a record in the same
   family as the VCOM record**: a magic, a schema version, a payload
   length, a CRC-32 trailer over everything preceding it, and an injected
   store seam. The payload is the 16 bytes of
   `ra8_gfx_tone_lut_t.level_gray8` in level order, so a record decodes
   straight into the published struct with no packing and no reordering.

2. **Resolution fails open to the nominal curve**, which is the opposite of
   the VCOM rule and for the reason stated above. Sources in descending
   authority: the per-device record, then a bench-supplied curve for this
   boot, then `k_ra8_gfx_tone_lut_nominal`. There is no dead end and no
   caller that has to refuse to render.

3. **Every candidate curve passes `ra8_gfx_tone_lut_validate` before use**,
   whatever supplied it. A record that fails the curve contract is rejected
   exactly like one that fails its CRC, and resolution reports which source
   answered, so a log or a service screen can distinguish "uncalibrated"
   from "calibration present and rejected". Those two demand different
   operator responses, the same argument
   `libs/ra8_epd_cal/inc/ra8_epd_cal.h` makes for splitting its own
   rejection codes.

4. **The record stores a normalised shape, not absolute reflectance.** The
   curve contract pins the endpoints at 0 and 255, so a stored curve says
   how the 14 interior levels sit between that panel's own black and its own
   white. Absolute contrast is a property of the glass and is not something
   the quantiser can act on, so it is deliberately not stored here.
   Normalising to the measured black and white is a step of the bench
   procedure below, not something firmware does on read.

5. **The record does not go in the extra-MRAM option-setting window.** It
   belongs in the general per-device configuration record that
   `libs/ra8_epd_cal/inc/ra8_epd_cal.h` reserves `0x40 .. 0x1BF` for, or in
   whatever rewritable medium issue #315 settles on. Until one of those
   exists, the only supported source of a measured curve is the
   bench-supplied one, and the record half of this decision is specified
   but not implemented.

6. **Provisioning is explicit.** A boot path never writes a tone record.
   Writing one is a service or provisioning action, as
   `ra8_epd_cal_provision` is for VCOM, so calibration cannot drift without
   somebody asking for it.

## Bench procedure

This is the procedure that has to be run to produce a curve. It is written
out because a measurement nobody can repeat is not a calibration, and
because every number it produces is one this repository will otherwise be
asked to take on trust.

1. **Clear, then drive a step wedge of all 16 levels** as large flat
   patches, not single pixels or thin bars, after a full-refresh clear and
   with the panel driven at its own resolved VCOM and its own waveform. A
   patch has to be large enough that the instrument's aperture sees only
   that patch.
2. **Measure reflectance per patch with an instrument**, under stated
   illumination and measurement geometry. A photograph of the screen
   measures the camera's own tone curve as much as the panel's and is not
   acceptable input to this record.
3. **Record the instrument, geometry, illumination and ambient
   temperature** alongside the curve. E-paper response is temperature
   dependent, so a curve measured warm is not the curve the reader uses in
   a cold room, and a curve with no stated temperature cannot be compared
   with the next one.
4. **Repeat across several units of the same panel model and record the
   spread.** This is the measurement that decides whether this record needs
   to exist at all: if the unit-to-unit spread is small next to the interval
   widths, the curve is a property of the panel model and belongs in the
   board or panel descriptor as a constant, and a per-device record is
   unnecessary machinery.
5. **Normalise to the measured black and white before storage**, per
   decision 4, so the stored knots satisfy the curve contract
   (`[0] == 0`, `[15] == 255`, strictly increasing). A measured response
   that is not strictly increasing after normalisation is a finding about
   the panel or the measurement, not a curve to round into shape.

## Consequences

* A measured panel can be mapped without touching the dither rule, and the
  committed goldens stay valid, because the nominal curve remains the
  default and reproduces the existing closed form exactly.
* One more per-device record, one more CRC and one more provisioning step.
  That cost mostly disappears if the general per-device configuration
  record lands first and this becomes 16 bytes of its payload.
* An operator can provision a wrong curve. The worst case is an ugly page,
  and recovery is a rewrite, which is the whole reason the policy here is
  fail open rather than fail closed.
* Decision 5 means the storage half cannot be implemented until issue #315
  or the general per-device record provides a rewritable medium. The
  bench-supplied path can be implemented immediately and is what first
  light will actually use.

## Open questions

1. Per-unit or per-panel-model. Step 4 of the bench procedure decides it,
   and a per-model answer retires decisions 1, 5 and 6 entirely.
2. Where a rewritable per-device record actually lives (issue #315).
3. Temperature: one curve, or a small family plus a compensation term.
   Nothing in this record assumes either.
4. Ageing: whether the response drifts enough over the life of a panel to
   need re-calibration, and what would trigger it.
5. Whether the curve has to be measured under a defined front-light
   setting. The warm/cool front light bounded by ADR-0007 mixes two colour
   channels over the same glass, and a tone curve measured under one
   setting may not describe another.
6. Which instrument class and illumination standard the procedure should
   name, rather than leaving it to whoever runs it.

## What this record does not contain

No measured number, no curve, no reflectance figure and no temperature.
No panel has been measured, nothing in this repository has seen one, and
nothing here is a measurement or an estimate of one.

## References

* Issue #479 -- per-panel gray-level LUT (perceptual 16-level tone mapping),
  labelled `needs-bench`.
* Issue #475 -- the e-ink render-quality epic this sits under.
* Issue #315 -- rewritable-medium home for per-device records.
* Draft pull request #1326 -- the curve format, nominal curve and renderer
  consumption.
* `libs/ra8_gfx/inc/ra8_gfx_tone.h` -- the curve and its contract.
* `libs/ra8_gfx/src/ra8_gfx_dither.c` -- the quantiser the curve feeds.
* `libs/ra8_epd_cal/inc/ra8_epd_cal.h` -- the per-device VCOM record whose
  record shape, store seam and resolution reporting this record follows,
  and whose fail-closed policy it deliberately does not.
* ADR-0007 -- the front-light driver interface referenced by open question 5.
