/**
 * @file examples/ra8p1_foundation/npu_vela/main.c
 * @brief RA8P1 Ethos-U55 .npub container loader end-to-end -- load a `.npub`, run it
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to ``examples/ra8p1_foundation/npu_smoke`` on the way to issue #227:
 * instead of hand-building a command stream in SRAM, this app LOADS a
 * committed, generated ``.npub`` model container
 * (``tools/vela/generated/ra8_npu_model_addk_fake.h``, produced offline by
 * ``tools/vela/vela_gen.py``) through the on-target loader ``ra8_npu_load()``,
 * which maps it into an ``ra8_npu_job_t`` -- command stream plus resolved region
 * bases (baked weights/input inside the blob, the output activation carved from a
 * runtime SRAM arena). It then submits + runs the job, reads the output arena
 * back, and asserts it holds the deterministic ``input + K`` result. The verdict
 * is printed over the SCI8 console:
 *
 * @code
 * npu-vela: id=0x10060000 load=OK run=OK out=0x........ verdict=PASS
 * @endcode
 *
 * The container's command stream is NOT a real Vela program: it is the tiny,
 * documented ra8_emulator / host-test convention in ``ra8_npu_fake_cmd.h``. Under
 * ``tools/ra8_emulator --device ra8p1`` the NPU model decodes it and applies the op
 * to the tensor arenas, so this app is a DETERMINISTIC, emulator-runnable proof of the
 * FULL offline-build -> on-target-load -> run pipeline. It is still a FOUNDATION
 * app: there is no RA8P1 board yet, and real Vela-compiled inference is the
 * follow-up on the RA8P1 NPU epic. On silicon the NPU reaches the arenas over
 * AXI, so a real driver would add cache maintenance around the tensors; the
 * cache-less emulator needs none, so this foundation omits it.
 *
 * @par Architectural ring
 * See docs/RING_AND_WORLD.md for what `[Ring 6 / APP] {World: S}` means --
 * application-layer code that runs in the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-15
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_device.h"
#include "ra8_err.h"
#include "ra8_npu.h"
#include "ra8_npu_blob.h"
#include "ra8_npu_fake_cmd.h"
#include "ra8_npu_loader.h"
#include "ra8_npu_model_addk_fake.h"
#include "ra8_npu_regs.h"

#ifndef RA8_HAS_NPU
#error "npu_vela must be built with cmake/toolchain-ra8p1.cmake (RA8_DEVICE_RA8P1)."
#endif

/**
 * @enum npu_vela_size_t
 * @brief Console baud, runtime-arena size, banner width, and word geometry.
 */
typedef enum : uint32_t {
  k_npu_vela_baud       = 115200U, /**< SCI8 J-Link OB console baud.         */
  k_npu_vela_arena      = 128U,    /**< Runtime arena for output activation. */
  k_npu_vela_line_cap   = 96U,     /**< Banner line buffer cap.              */
  k_npu_vela_word_bytes = 4U,      /**< Bytes in one little-endian word.     */
  k_npu_vela_byte_mask  = 0xFFU,   /**< 8-bit element wrap (matches the op). */
} npu_vela_size_t;

/**
 * @enum npu_vela_fnv_t
 * @brief FNV-1a 32-bit constants for the displayed output checkword.
 */
typedef enum : uint32_t {
  k_npu_vela_fnv_offset = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis. */
  k_npu_vela_fnv_prime  = 0x01000193U, /**< FNV-1a 32-bit prime.        */
} npu_vela_fnv_t;

/**
 * @var s_npu_vela_arena
 * @brief Runtime arena the loader carves the output activation from.
 * @details Passed to ra8_npu_load() as the runtime region backing store; the
 *          NPU (fake) writes the model output here.
 * @note Written by the NPU (fake), read back by the app.
 * @since 0.1.0
 */
static uint8_t s_npu_vela_arena[k_npu_vela_arena];

/**
 * @var g_npu_vela_id
 * @brief NPU_ID captured after init, for external (J-Link) inspection.
 * @details `volatile` + non-static so a debugger can read it; firmware writes once.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_vela_id = 0U;

/**
 * @var g_npu_vela_check
 * @brief Output checkword captured after verify, for external inspection.
 * @details `volatile` + non-static so a debugger can read the result digest.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_vela_check = 0U;

/**
 * @var g_npu_vela_pass
 * @brief Final verdict (1 = PASS, 0 = FAIL), for external inspection.
 * @details `volatile` + non-static so a memprobe can read the verdict.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_vela_pass = 0U;

/**
 * @brief Park the CPU forever in WFI after a fatal init error (a real panic).
 *
 * @details Reserved for failures BEFORE a verdict can be emitted (CGC or the
 *          SCI8 console did not come up), so a ra8_emulator gate that scans for a
 *          `*panic_halt` terminal PC correctly reads this as a failed run.
 *
 * @pre Called only after a fatal init error, before the verdict banner.
 * @pre No initialized diagnostic path can publish a normal verdict.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @post No NPU loader, driver, or console operation follows entry.
 * @note Emulator automation uses the panic suffix to classify boot failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_vela_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Park the CPU forever in WFI after the verdict banner (a clean stop).
 *
 * @details Distinct from ::internal_npu_vela_panic_halt on purpose: a run that reached the
 *          verdict has done its job and parks here, whose name deliberately does
 *          NOT match the `*panic_halt` patterns a ra8_emulator gate treats as a
 *          give-up. The authoritative verdict is the emitted banner.
 *
 * @pre The verdict banner has been emitted over the SCI8 console.
 * @pre Published ID, checkword, and pass values are final.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @post Run evidence remains stable for debugger or memory-probe inspection.
 * @note The banner, rather than this terminal PC, determines PASS or FAIL.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_vela_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up CGC + the SCI8 console. Panic-halts on any failure.
 *
 * @details The NPU needs no clock beyond the CGC default NPUCLK; only the
 *          console (PCLKA/SCICLK) is set up here so the verdict can be printed.
 *
 * @pre Reset_Handler has initialised .data / .bss.
 * @pre The application remains in single-threaded boot context.
 * @post On return CGC is up and the SCI8 console is ready for writes.
 * @post Any mandatory setup error transfers to the panic halt.
 * @note NPU initialization is deferred until diagnostics are known operational.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_vela_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_npu_vela_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_npu_vela_baud) != k_ra8_ok) {
    internal_npu_vela_panic_halt();
  }
}

/**
 * @brief Read a little-endian command-stream word from the loaded job.
 *
 * @details Computes the byte offset for the requested fake-command word and
 * delegates endian-safe decoding to the blob reader.
 * @param[in] job   Loaded job whose cmd_stream is byte-addressable.
 * @param[in] widx  Word index within the SE55 command stream.
 * @return The 32-bit word at @p widx.
 * @retval 0..UINT32_MAX The decoded little-endian command word.
 *
 * @pre job->cmd_stream is non-NULL and at least (widx+1) words long.
 * @pre ``widx`` names a defined word within the loaded command header.
 * @post No state is modified; the function is pure.
 * @post The job's stream position and region mappings remain unchanged.
 * @note The helper intentionally performs no bounds check beyond its contract.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_npu_vela_cmd_word(const ra8_npu_job_t*    job,
                                                        ra8_npu_fake_word_idx_t widx)
{
  return ra8_npu_blob_read_word((const uint8_t*)job->cmd_stream,
                                (uint32_t)widx * (uint32_t)k_npu_vela_word_bytes);
}

/**
 * @brief Verify the output arena equals input+K and fold it into a checkword.
 *
 * @details Reads the resolved input region (baked in the blob) and output region
 *          (the runtime arena) straight out of the loaded job, plus the add
 *          constant and byte count from the command stream, and checks every
 *          output byte against the deterministic `input + K` result.
 *
 * @param[in]  job       Job returned by ra8_npu_load() (regions resolved).
 * @param[out] out_check FNV-1a digest of the output bytes (for the banner).
 * @return true when every output byte matches the expected add-constant result.
 * @retval true Every byte matches the command stream's add-constant operation.
 * @retval false One or more bytes differ from the deterministic expectation.
 *
 * @pre out_check is non-NULL; the job has completed.
 * @pre Input and output region mappings cover the command's declared byte count.
 * @post *out_check holds the output digest regardless of the verdict.
 * @post The job, command stream, and data regions are not modified.
 * @note The FNV digest is diagnostic evidence rather than an integrity primitive.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_npu_vela_verify(const ra8_npu_job_t* job, uint32_t* out_check)
{
  const uint8_t* input  = (const uint8_t*)(uintptr_t)job->region_base[k_ra8_npu_region_1];
  const uint8_t* output = (const uint8_t*)(uintptr_t)job->region_base[k_ra8_npu_region_2];
  const uint32_t konst  = internal_npu_vela_cmd_word(job, k_ra8_npu_fake_word_const);
  const uint32_t count  = internal_npu_vela_cmd_word(job, k_ra8_npu_fake_word_count);
  uint32_t       check  = (uint32_t)k_npu_vela_fnv_offset;
  bool           ok     = true;
  for (uint32_t i = 0U; i < count; i++) {
    const uint8_t expected = (uint8_t)((input[i] + konst) & (uint32_t)k_npu_vela_byte_mask);
    if (output[i] != expected) {
      ok = false;
    }
    check = (check ^ (uint32_t)output[i]) * (uint32_t)k_npu_vela_fnv_prime;
  }
  *out_check = check;
  return ok;
}

/**
 * @brief Load the golden blob, submit + run + wait for the NPU job.
 *
 * @details Resolves the generated container into the static arena, submits its
 * descriptor, starts the NPU, and waits synchronously for completion.
 * @param[out] out_job Filled with the loaded, run job descriptor on success.
 * @return `ra8_err_t` from the first failing loader / driver call, else k_ra8_ok.
 * @retval k_ra8_ok The container loaded and the NPU job completed.
 * @retval non-k_ra8_ok The loader, submission, run, or wait stage failed.
 *
 * @pre out_job is non-NULL; ra8_npu_init() previously succeeded.
 * @pre The static Vela arena is not owned by another job.
 * @post On k_ra8_ok the output region of *out_job holds the NPU result.
 * @post On failure no later loader or driver stage is attempted.
 * @note Region storage remains application-owned for the complete synchronous run.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_npu_vela_run_job(ra8_npu_job_t* out_job)
{
  const ra8_npu_arena_t arena = {.base = s_npu_vela_arena, .bytes = (uint32_t)k_npu_vela_arena};
  const ra8_err_t       ld =
    ra8_npu_load(ra8_npu_model_addk_fake_blob(), ra8_npu_model_addk_fake_bytes(), &arena, out_job);
  if (ld != k_ra8_ok) {
    return ld;
  }
  const ra8_err_t sub = ra8_npu_submit(out_job);
  if (sub != k_ra8_ok) {
    return sub;
  }
  const ra8_err_t run = ra8_npu_run();
  if (run != k_ra8_ok) {
    return run;
  }
  return ra8_npu_wait();
}

/**
 * @brief Format and print the one-line verdict banner over the SCI8 console.
 *
 * @details Formats load, run, output, and verdict evidence into a bounded local
 * line, then writes and flushes it when formatting succeeds.
 * @param[in] id      NPU_ID read after init.
 * @param[in] load_ok Whether ra8_npu_load() succeeded.
 * @param[in] run_ok  Whether the submit/run/wait sequence returned k_ra8_ok.
 * @param[in] check   Output checkword from internal_npu_vela_verify().
 * @param[in] pass    Final verdict (id valid AND load AND run AND output matched).
 *
 * @pre The SCI8 console is initialised.
 * @pre All supplied status values are final snapshots for this run.
 * @post One banner line has been written and flushed to the console.
 * @post The NPU job, arena, and published globals remain unchanged.
 * @note A nonpositive formatting result safely suppresses console output.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_npu_vela_emit(uint32_t id, bool load_ok, bool run_ok, uint32_t check, bool pass)
{
  char      line[k_npu_vela_line_cap];
  const int n = snprintf(line,
                         sizeof(line),
                         "npu-vela: id=0x%08X load=%s run=%s out=0x%08X verdict=%s\r\n",
                         (unsigned)id,
                         load_ok ? "OK" : "FAIL",
                         run_ok ? "OK" : "FAIL",
                         (unsigned)check,
                         pass ? "PASS" : "FAIL");
  if (n > 0) {
    (void)ra8_board_uart_console_write((const uint8_t*)line, (size_t)n);
    (void)ra8_board_uart_console_flush();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: load a `.npub` model, run it, print the verdict.
 *
 * @return Never returns (halts in WFI after emitting the banner).
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post The verdict banner is on the console; the CPU is parked in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_npu_vela_setup_or_halt();

  bool     load_ok = false;
  bool     run_ok  = false;
  uint32_t id      = 0U;
  uint32_t check   = 0U;
  bool     pass    = false;

  if (ra8_npu_init() == k_ra8_ok) {
    (void)ra8_npu_read_id(&id);
    g_npu_vela_id = id;

    ra8_npu_job_t   job = {};
    const ra8_err_t rc  = internal_npu_vela_run_job(&job);
    load_ok             = (job.cmd_stream != nullptr); /* load leaves it null on failure */
    run_ok              = (rc == k_ra8_ok);

    /* Gate verify on load_ok as well: a successful run implies the load
     * populated cmd_stream, but stating it explicitly lets the static analyzer
     * see that internal_npu_vela_verify() never dereferences a null cmd_stream. */
    const bool matched = run_ok && load_ok && internal_npu_vela_verify(&job, &check);
    g_npu_vela_check   = check;
    pass               = load_ok && run_ok && matched && (id != 0U);
  }

  g_npu_vela_pass = pass ? 1U : 0U;
  internal_npu_vela_emit(id, load_ok, run_ok, check, pass);

  internal_npu_vela_park();
  return 0;
}
#pragma GCC diagnostic pop
