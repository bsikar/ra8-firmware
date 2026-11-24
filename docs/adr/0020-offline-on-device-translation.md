# ADR-0020: On-device offline translation as a background chapter job (no LLM, no companion chip for v1)

## Status

Accepted -- 2026-10-04 (recorded 2026-11-24). Records the architectural
decision for `RA8FW-660` ("On-device offline page translation for manga
and webtoons on RA8P1") approved by the repository owner on 2026-10-04.

## Context

The RA8P1 target pairs a Cortex-M85 with an Arm Ethos-U55 micro-NPU and
drives an e-ink reader shelf. Supporting translated manga and webtoon
reading requires turning downloaded chapter pages (Korean, Japanese, and
Chinese) into overlaid English bubbles: text/bubble detection, OCR,
language identification, translation, name consistency across chapters,
and e-ink overlay rendering.

The architecture had to resolve five linked questions:

1. **Offline vs. cloud:** Cloud models such as Gemini Flash require
   network connectivity and a remote service for every chapter or page.
2. **Translator model class:** Even the smallest open LLMs (such as
   Gemma 3 270M at ~125 MB in int4) are autoregressive decoder-only
   models that stream all weights once per generated output token,
   putting throughput in seconds per token within microcontroller
   external-memory bandwidth.
3. **Timing of translation:** Running detection, OCR, and translation on
   demand when a reader turns a page adds multi-second latency per page
   flip and fragments glossary context across isolated bubbles.
4. **Companion processor vs. single-MCU v1:** Offloading an LLM to an
   on-board companion chip capable of running multi-hundred-megabyte
   models requires a Linux-class SoC with gigabytes of LPDDR, a second
   firmware stack to build, boot, and update, and high standby/active
   power draw on an e-ink battery budget.
5. **NPU container format:** Bare-metal firmware in this repository uses
   pre-distilled `.npub` containers (`libs/ra8_hal/inc/ra8_npu_blob.h`)
   emitted offline by `tools/vela/src/vela_gen.py` and executed by the
   first-party `ra8_npu` driver (`libs/ra8_hal/inc/ra8_npu.h`), rather
   than parsing TFLite flatbuffers on device.

## Decision

1. **Fully offline on-device pipeline (no LLM, no server, no cloud):**
   Every stage -- detection, OCR, script/language selection, machine
   translation, and bubble overlay -- executes locally on the RA8P1 with
   no cloud API or companion server requirement.
2. **Bergamot-class int8 Marian NMT translator:** Use compact int8
   Marian NMT encoder-decoder models (Mozilla Firefox Translations /
   Bergamot class, MPL-2.0, ~17 to 55 MB per language pair) executed by
   a freestanding first-party Zig transformer runtime reading weights
   from external memory. Both cloud models (Gemini Flash) and small
   on-device LLMs (Gemma 3 270M int4) are rejected for v1.
3. **Background chapter job after MDL download:** Execute the full
   chapter translation as a background job triggered once MDL completes a
   chapter download, rather than at page-turn time. Processing a full
   chapter at once allows:
   - a single per-series glossary with placeholder substitution to keep
     character names and recurring terms consistent across bubbles and
     chapters;
   - batched decoding across all extracted bubbles in the chapter so
     model weights stream once per decode step instead of once per
     bubble; and
   - instant page display during reading from precomputed chapter
     sidecars (`docs/formats/` + `ra8_widget` overlay).
4. **No companion chip for v1 behind a clean translator boundary:** Ship
   v1 solely on the RA8P1 MCU without a Linux-class companion SoC and
   gigabytes of LPDDR. Keep the translation engine behind one narrow
   interface (`source text + glossary -> English text`) so a future
   companion processor or an optional host/Proxmox LLM pass can be
   inserted behind the same boundary without changing the chapter-job
   pipeline or sidecar consumer.
5. **Detection and OCR via `.npub` on Ethos-U55; no TFLite Micro on device:**
   Run text/bubble detection and script-specific CTC OCR recognizers
   (Korean, Japanese, Chinese) on the Ethos-U55 through offline
   Vela-compiled `.npub` blobs mapped directly by the `ra8_npu` loader,
   avoiding an on-device TFLite Micro runtime in this pipeline. Choose
   the recognizer and translator from Unicode script ranges on sampled
   text combined with the per-title MDL language hint rather than adding
   a learned language-ID model. Host Python tooling (PyTorch/TensorFlow,
   TFLite converter, and Vela) stays strictly offline at build/asset-prep
   time.

## Alternatives Considered

* **Cloud LLM API (Gemini Flash):** Higher raw translation ceiling, but
  requires always-on connectivity, accounts/keys, and external servers.
  Rejected as incompatible with offline e-reader hardware.
* **Small open LLM on MCU (Gemma 3 270M int4, ~125 MB):** Exceeds
  practical latency and external-memory bandwidth budgets by reading all
  weights on every output token. Rejected in favor of 17-55 MB int8
  encoder-decoder Marian NMT.
* **Companion Linux SoC in v1:** Adds BOM cost, GBs of LPDDR, a second
  OS/firmware stack, and severe battery penalty for e-ink reading.
  Deferred cleanly behind the narrow translator interface.
* **On-demand page-time translation:** Hides no model-streaming cost on
  page turns and weakens cross-bubble name consistency. Rejected in
  favor of batched background chapter jobs after MDL download.

## Consequences

* **Positive:**
  * Readers experience zero translation wait during reading while keeping
    hardware power, memory, and firmware complexity within a single MCU.
  * Batched chapter decode amortizes external-memory weight reads across
    multiple bubbles per decode step, and series glossaries mitigate NMT
    consistency limits without an LLM.
  * Detection and OCR reuse the existing `.npub` container and `ra8_npu`
    driver foundation without adding TFLite flatbuffer parsing on device.
* **Negative / Trade-offs:**
  * Int8 NMT output quality is narrower in idiomatic nuance and document
    context than an LLM, making placeholder glossary substitution and
    pre-batching essential for acceptable chapter continuity.
  * Chapters wait for background translation completion after download
    before translated overlays appear immediately.
  * Supporting multiple source languages requires storing 17 to 55 MB per
    active language pair plus per-script OCR `.npub` models in external
    storage.
* **Preserved Upgrade Path:** Encapsulating translation behind `source
  text + glossary -> English text` allows future hardware revisions with
  a companion chip or offline Proxmox batch refinement to reuse the same
  detector, OCR, glossary, sidecar format, and UI overlay layers.

## References

* `RA8FW-660` -- parent epic for offline manga and webtoon translation
  (`RA8FW-1183`, `RA8FW-1184`, `RA8FW-1185`, `RA8FW-1186`, and follow-on
  Zig runtime slices).
* `RA8FW-663` -- ADR tracking issue for this architectural record.
* `libs/ra8_hal/inc/ra8_npu_blob.h` -- `.npub` Ethos-U55 container
  specification consumed by the `ra8_npu` loader.
* `libs/ra8_hal/inc/ra8_npu.h` -- first-party Ethos-U55 command-queue
  driver foundation (`ra8_npu_submit()`).
* `tools/vela/README.md` and `tools/vela/src/vela_gen.py` -- host-only
  Vela-to-`.npub` container distillation tooling.
