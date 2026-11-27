# translate_eval_e2e

Target Cortex-M85 end-to-end firmware runner for `libs/ra8_translate` and `tools/translate_eval`.

## What It Exercises On Target

1. **Chapter Job & `TRJC` Reset/Restore Gate** (always runs at boot):
   - Initializes `ra8_translate_job` with explicit battery (`>= 25%` off-charger, charger override) and foreground reader yield policies plus unread-first chapter ordering.
   - Executes real C ABI quantized `int8_t` detector post-processing (`ra8_translate_det_post_i8` including furigana drop), quantized `int8_t` CTC OCR decoding (`ra8_translate_ctc_decode_i8`), series `TGL1` glossary extraction/masking/restoration, Unigram `TVOC` tokenization, greedy `TMNT` transformer decoding (`ra8_translate_marian_greedy_decode`), `ra8_gfx` overlay text fitting (`ra8_translate_overlay_fit_text`), and `TRN1` sidecar serialization.
   - Captures a mid-chapter `TRJC` checkpoint after page 0, zeroes the scheduler workspace to simulate a power cut, restores from `TRJC`, completes the chapter, and verifies bit-exact `TRN1` equality against the uninterrupted run.
2. **External `TPAK.BIN` SD Corpus Evaluation** (runs when `--sd-dir` or a microSD card supplies `/TPAK.BIN`):
   - Mounts the SD card through `ra8_sdcard` (`SDHI1_B`) and `ra8_fs`, validates the CRC-32 `TPAK` v1 container in SDRAM, executes detector -> CTC OCR -> Marian NMT on every page and bubble on target ARM code, and prints structured `EMU_TRACE_BEGIN..EMU_TRACE_END` JSON over UART for `tools/translate_eval`.
   - When no SD card or `TPAK.BIN` is present (e.g., standalone smoke tests), runs an in-memory self-check `TPAK` bundle and still reports `translate_eval_e2e: PASS` before `translate_eval_e2e: END`.
