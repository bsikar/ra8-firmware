/**
 * @file examples/ra8p1_foundation/npu_smoke/main.c
 * @brief RA8P1 Arm Ethos-U55 NPU foundation smoke -- runs a checkable stand-in job
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to ``examples/ra8p1_foundation/blink_ra8p1`` for the NPU. This app
 * drives the FULL ``ra8_npu`` command/queue surface end-to-end and CHECKS the
 * result: bring the Ethos-U55 out of module-stop (``ra8_npu_init``), read the
 * ``NPU_ID`` probe, build a command stream + input tensor in SRAM, program them
 * (``ra8_npu_submit``), kick the job (``ra8_npu_run``), wait for completion
 * (``ra8_npu_wait``), then READ the output arena back and assert it holds the
 * deterministic expected result. The verdict is printed over the SCI8 console:
 *
 * @code
 * npu: id=0x10060000 run=OK out=0x........ verdict=PASS
 * @endcode
 *
 * The SCI8 console is provided by the dedicated ``ra8_board_ra8p1`` board layer
 * (issue #226), whose LED/switch/console pins are provisional (mirrored from the
 * pin-compatible EK-RA8D2) until an RA8P1 board is defined -- see that layer's
 * header for the ``TODO(EK-RA8P1 UM / ra8p1_kicad)`` rationale.
 *
 * The command stream is NOT a real Vela program: it uses the tiny, documented
 * ra8_emulator / host-test convention in ``ra8_npu_fake_cmd.h`` (an "SE55" magic word
 * plus an add-constant opcode). Under ``tools/ra8_emulator --device ra8p1`` the NPU
 * model decodes it and applies the op to the tensor arenas, so this app is a
 * DETERMINISTIC, emulator-runnable check of the driver protocol + BASEPn region
 * programming -- run it twice and the banner is identical. It is still a
 * FOUNDATION app: there is no RA8P1 board yet, and real Vela-compiled inference
 * is the follow-up on the RA8P1 NPU epic. On silicon the NPU reaches the arenas
 * over AXI, so a real driver would add cache maintenance around the tensors; the
 * cache-less emulator needs none, so this foundation omits it.
 *
 * @par Architectural ring
 * See docs/RING_AND_WORLD.md for what `[Ring 6 / APP] {World: S}` means --
 * application-layer code that runs in the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-10
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_boot_entry.h"
#include "ra8_attributes.h"
#include "ra8_board_ra8p1.h"
#include "ra8_cgc.h"
#include "ra8_device.h"
#include "ra8_err.h"
#include "ra8_npu.h"
#include "ra8_npu_fake_cmd.h"
#include "ra8_npu_regs.h"

/*
 * Compile-time proof that the RA8P1 toolchain selection reached this TU: the NPU
 * driver only exists when RA8_HAS_NPU is defined (RA8_DEVICE_RA8P1). Building this
 * app with the RA8D2 toolchain fails loudly here instead of silently dropping
 * the NPU.
 */
#ifndef RA8_HAS_NPU
#error "npu_smoke must be built with cmake/toolchain-ra8p1.cmake (RA8_DEVICE_RA8P1)."
#endif

/**
 * @enum npu_smoke_size_t
 * @brief Command-stream / tensor-arena sizes and the console baud.
 */
typedef enum : uint32_t {
  k_npu_smoke_baud        = 115200U,                 /**< SCI8 J-Link OB console baud. */
  k_npu_smoke_arena_bytes = 64U,                     /**< Tensor-arena length (bytes). */
  k_npu_smoke_cmd_words   = k_ra8_npu_fake_word_num, /**< Command-stream word count.   */
  k_npu_smoke_line_cap    = 80U,                     /**< Banner line buffer cap.      */
} npu_smoke_size_t;

/**
 * @enum npu_smoke_region_t
 * @brief Tensor-region indices the smoke job programs into BASEPn.
 */
typedef enum : uint8_t {
  k_npu_smoke_region_weights = 0U, /**< Region 0: Vela weight arena (unused by op). */
  k_npu_smoke_region_input   = 1U, /**< Region 1: op source (input tensor).         */
  k_npu_smoke_region_output  = 2U, /**< Region 2: op destination (output tensor).   */
  k_npu_smoke_region_count   = 3U, /**< Regions programmed (BASEP0..2).             */
} npu_smoke_region_t;

/**
 * @enum npu_smoke_seed_t
 * @brief Deterministic input pattern + the add-constant the job applies.
 */
typedef enum : uint8_t {
  k_npu_smoke_addk      = 0x11U, /**< Constant added to every input byte.  */
  k_npu_smoke_seed_mul  = 7U,    /**< input[i] = (i * mul + add) & mask.   */
  k_npu_smoke_seed_add  = 3U,    /**< Input pattern additive offset.       */
  k_npu_smoke_byte_mask = 0xFFU, /**< 8-bit element wrap (matches the op). */
} npu_smoke_seed_t;

/**
 * @enum npu_smoke_fnv_t
 * @brief FNV-1a 32-bit constants for the displayed output checkword.
 */
typedef enum : uint32_t {
  k_npu_smoke_fnv_offset = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis. */
  k_npu_smoke_fnv_prime  = 0x01000193U, /**< FNV-1a 32-bit prime.        */
} npu_smoke_fnv_t;

/**
 * @var s_npu_cmd_stream
 * @brief Stand-in command stream in SRAM (ra8_npu_fake_cmd.h layout; add-constant op).
 * @details QBASE/QSIZE point the NPU at this; the fake model decodes it.
 * @note Not a real Vela program -- see the file header.
 * @since 0.1.0
 */
static uint32_t s_npu_cmd_stream[k_npu_smoke_cmd_words];

/**
 * @var s_npu_weights
 * @brief Region 0 weight arena (present by Vela convention; unused by this op).
 * @details Programmed into BASEP0 so the region layout matches a real job.
 * @note Contents irrelevant to the add-constant op.
 * @since 0.1.0
 */
static uint8_t s_npu_weights[k_npu_smoke_arena_bytes];

/**
 * @var s_npu_input
 * @brief Region 1 input tensor: seeded with a deterministic byte pattern.
 * @details The NPU (fake) reads this and writes input+K to the output arena.
 * @note Seeded by internal_npu_smoke_seed_arenas().
 * @since 0.1.0
 */
static uint8_t s_npu_input[k_npu_smoke_arena_bytes];

/**
 * @var s_npu_output
 * @brief Region 2 output tensor: zeroed pre-run, holds the NPU result post-run.
 * @details Verified byte-for-byte against the expected input+K result.
 * @note Written by the NPU (fake), read back by the app.
 * @since 0.1.0
 */
static uint8_t s_npu_output[k_npu_smoke_arena_bytes];

/**
 * @var g_npu_smoke_id
 * @brief NPU_ID captured after init, for external (J-Link) inspection.
 * @details `volatile` + non-static so a debugger can read it; firmware writes once.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_smoke_id = 0U;

/**
 * @var g_npu_smoke_status
 * @brief Raw NPU_STATUS captured after the run, for external inspection.
 * @details `volatile` + non-static so a debugger can watch it.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_smoke_status = 0U;

/**
 * @var g_npu_smoke_check
 * @brief Output checkword captured after verify, for external inspection.
 * @details `volatile` + non-static so a debugger can read the result digest.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_smoke_check = 0U;

/**
 * @var g_npu_smoke_pass
 * @brief Final verdict (1 = PASS, 0 = FAIL), for external inspection.
 * @details `volatile` + non-static so a memprobe can read the verdict.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_smoke_pass = 0U;

/**
 * @brief Park the CPU forever in WFI after a fatal init error (a real panic).
 *
 * @details Reserved for failures BEFORE a verdict can be emitted (CGC or the
 *          SCI8 console did not come up), so a ra8_emulator gate that scans for a
 *          `*panic_halt` terminal PC correctly reads this as a failed run. The
 *          normal post-verdict terminal is ::internal_npu_smoke_park, which is NOT a
 *          panic and must not be flagged as one.
 *
 * @pre Called only after a fatal init error, before the verdict banner.
 * @pre No initialized console path can publish a normal run verdict.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @post No NPU or console access occurs after entry.
 * @note The panic suffix is consumed by emulator failure classification.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_smoke_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Park the CPU forever in WFI after the verdict banner (a clean stop).
 *
 * @details Distinct from ::internal_npu_smoke_panic_halt on purpose: a run that reached
 *          the verdict -- PASS or FAIL -- has done its job and parks here, whose
 *          name deliberately does NOT match the `*panic_halt` / `*_halt_loop`
 *          patterns a ra8_emulator gate treats as a give-up. The authoritative
 *          verdict is the emitted `verdict=PASS` / `verdict=FAIL` banner (and
 *          ::g_npu_smoke_pass for a memprobe), never the parked PC.
 *
 * @pre The verdict banner has been emitted over the SCI8 console.
 * @pre Run-result globals contain their final diagnostic values.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @post The verdict remains stable for an attached memory probe.
 * @note Reaching this clean terminal does not imply PASS; the banner is authoritative.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_smoke_park(void)
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
 * @note NPU initialization is intentionally deferred until after diagnostics work.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_smoke_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_npu_smoke_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_npu_smoke_baud) != k_ra8_ok) {
    internal_npu_smoke_panic_halt();
  }
}

/**
 * @brief Fill the command stream (add-constant op) per the stand-in convention.
 *
 * @details Writes the five header words of ra8_npu_fake_cmd.h: magic|opcode,
 *          source region, destination region, byte count, constant addend.
 *
 * @pre s_npu_cmd_stream has k_ra8_npu_fake_word_num words.
 * @pre No active NPU job is reading the command stream.
 * @post s_npu_cmd_stream describes an add-constant of region 1 -> region 2.
 * @post Only the defined fake-command header words are modified.
 * @note The stream exercises driver plumbing with a deterministic stand-in op.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_smoke_build_stream(void)
{
  s_npu_cmd_stream[k_ra8_npu_fake_word_op] =
    (uint32_t)k_ra8_npu_fake_magic | (uint32_t)k_ra8_npu_fake_op_addk;
  s_npu_cmd_stream[k_ra8_npu_fake_word_src]   = (uint32_t)k_npu_smoke_region_input;
  s_npu_cmd_stream[k_ra8_npu_fake_word_dst]   = (uint32_t)k_npu_smoke_region_output;
  s_npu_cmd_stream[k_ra8_npu_fake_word_count] = (uint32_t)k_npu_smoke_arena_bytes;
  s_npu_cmd_stream[k_ra8_npu_fake_word_const] = (uint32_t)k_npu_smoke_addk;
}

/**
 * @brief Seed the input arena with a deterministic pattern and zero the output.
 *
 * @details Generates the fixed affine byte sequence used by verification and
 * clears every output byte so stale data cannot produce a false pass.
 * @pre The arenas are k_npu_smoke_arena_bytes long.
 * @pre The current run exclusively owns both static arenas.
 * @post s_npu_input holds the pattern; s_npu_output is all zero.
 * @post The weights and command-stream regions remain unchanged.
 * @note Deterministic seeding makes the emitted checkword reproducible.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_npu_smoke_seed_arenas(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_npu_smoke_arena_bytes; i++) {
    s_npu_input[i] =
      (uint8_t)(((i * (uint32_t)k_npu_smoke_seed_mul) + (uint32_t)k_npu_smoke_seed_add) &
                (uint32_t)k_npu_smoke_byte_mask);
    s_npu_output[i] = 0U;
  }
}

/**
 * @brief Submit + run + wait for the NPU job.
 *
 * @details Builds a descriptor over the static regions, submits it, starts the
 * NPU, and synchronously returns the completion wait result.
 * @return `ra8_err_t` from the first failing driver call, else k_ra8_ok.
 * @retval k_ra8_ok Job completed (command stream consumed).
 *
 * @pre The command stream and arenas are populated.
 * @pre The NPU is initialized and has no active job.
 * @post On k_ra8_ok the output arena holds the NPU result.
 * @post On failure no later driver stage is invoked by this helper.
 * @note All referenced region storage remains owned by the application.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_npu_smoke_run_job(void)
{
  ra8_npu_job_t job                           = {};
  job.cmd_stream                              = s_npu_cmd_stream;
  job.cmd_stream_bytes                        = (uint32_t)sizeof(s_npu_cmd_stream);
  job.region_count                            = (uint8_t)k_npu_smoke_region_count;
  job.region_base[k_npu_smoke_region_weights] = (uint64_t)(uintptr_t)s_npu_weights;
  job.region_base[k_npu_smoke_region_input]   = (uint64_t)(uintptr_t)s_npu_input;
  job.region_base[k_npu_smoke_region_output]  = (uint64_t)(uintptr_t)s_npu_output;

  const ra8_err_t sub = ra8_npu_submit(&job);
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
 * @brief Verify the output arena equals input+K and fold it into a checkword.
 *
 * @details Compares every output byte to the wrapped add-constant expectation
 * while accumulating an FNV-1a digest independent of the verdict.
 * @param[out] out_check FNV-1a digest of the output bytes (for the banner).
 * @return true when every output byte matches the expected add-constant result.
 * @retval true Every result byte matches its expected value.
 * @retval false One or more result bytes differ.
 *
 * @pre out_check is non-NULL; the job has completed.
 * @pre Input and output arenas contain the same completed run.
 * @post *out_check holds the output digest regardless of the verdict.
 * @post Neither arena is modified during verification.
 * @note The digest is diagnostic evidence, not a cryptographic authenticator.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_npu_smoke_verify(uint32_t* out_check)
{
  uint32_t check = (uint32_t)k_npu_smoke_fnv_offset;
  bool     ok    = true;
  for (uint32_t i = 0U; i < (uint32_t)k_npu_smoke_arena_bytes; i++) {
    const uint8_t expected =
      (uint8_t)((s_npu_input[i] + (uint32_t)k_npu_smoke_addk) & (uint32_t)k_npu_smoke_byte_mask);
    if (s_npu_output[i] != expected) {
      ok = false;
    }
    check = (check ^ (uint32_t)s_npu_output[i]) * (uint32_t)k_npu_smoke_fnv_prime;
  }
  *out_check = check;
  return ok;
}

/**
 * @brief Format and print the one-line verdict banner over the SCI8 console.
 *
 * @details Formats the device ID, driver result, output digest, and verdict into
 * a bounded local line, then writes and flushes it once.
 * @param[in] id     NPU_ID read after init.
 * @param[in] run_ok Whether the submit/run/wait sequence returned k_ra8_ok.
 * @param[in] check  Output checkword from internal_npu_smoke_verify().
 * @param[in] pass   Final verdict (id valid AND run_ok AND output matched).
 *
 * @pre The SCI8 console is initialised.
 * @pre All supplied values are final snapshots for the completed run.
 * @post One banner line has been written and flushed to the console.
 * @post NPU arenas and published result globals remain unchanged.
 * @note A nonpositive formatter result suppresses output safely.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_npu_smoke_emit(uint32_t id, bool run_ok, uint32_t check, bool pass)
{
  char      line[k_npu_smoke_line_cap];
  const int n = snprintf(line,
                         sizeof(line),
                         "npu: id=0x%08X run=%s out=0x%08X verdict=%s\r\n",
                         (unsigned)id,
                         run_ok ? "OK" : "FAIL",
                         (unsigned)check,
                         pass ? "PASS" : "FAIL");
  if (n > 0) {
    (void)ra8_board_uart_console_write((const uint8_t*)line, (size_t)n);
    (void)ra8_board_uart_console_flush();
  }
}

/**
 * @brief Application entry: bring up the NPU, run a checkable job, print verdict.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post The verdict banner is on the console; the CPU is parked in WFI.
 * @since 0.1.0
 */
void main(void)
{
  internal_npu_smoke_setup_or_halt();

  bool     run_ok = false;
  uint32_t id     = 0U;
  uint32_t check  = 0U;
  bool     pass   = false;

  if (ra8_npu_init() == k_ra8_ok) {
    (void)ra8_npu_read_id(&id);
    g_npu_smoke_id = id;

    internal_npu_smoke_build_stream();
    internal_npu_smoke_seed_arenas();
    run_ok = (internal_npu_smoke_run_job() == k_ra8_ok);

    ra8_npu_status_t st = {};
    if (ra8_npu_read_status(&st) == k_ra8_ok) {
      g_npu_smoke_status = st.raw;
    }
    const bool matched = internal_npu_smoke_verify(&check);
    g_npu_smoke_check  = check;
    pass               = run_ok && matched && (id != 0U);
  }

  g_npu_smoke_pass = pass ? 1U : 0U;
  internal_npu_smoke_emit(id, run_ok, check, pass);

  internal_npu_smoke_park();
}
