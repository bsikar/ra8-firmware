/**
 * @file examples/ra8p1_foundation/npu_infer/main.c
 * @brief RA8P1 Ethos-U55 inference runtime smoke -- quantize + IRQ + TFLite-micro
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The inference-runtime companion to ``examples/ra8p1_foundation/npu_smoke``
 * (which exercises the bare ``ra8_npu`` command/queue driver). This app drives
 * the pieces a real TensorFlow Lite for Microcontrollers Ethos-U runtime needs
 * on top of the driver, and CHECKS each end-to-end on the RA8P1 profile:
 *
 *  1. **Input/output quantization** (``ra8_npu_quant``): a float input arena is
 *     quantized to the UINT8 tensor the NPU reads, and the UINT8 result is
 *     dequantized back to float.
 *  2. **IRQ-driven completion** (``ra8_npu_irq_arm`` / ``ra8_npu_wait_irq`` +
 *     ``ra8_npu_irq_handler`` routed through ``ra8_isr``): the job is awaited on
 *     the NPU interrupt (``ra8_npu_clear_irq`` from the ISR), not a busy-wait.
 *  3. **TFLite-micro Ethos-U operator wiring** (``ra8_ethosu_kernel_available``):
 *     asserts the real first-party ``tflite::Register_ETHOSU()`` is linked and
 *     registered (the vendored stub returned ``nullptr``). This app links the
 *     vendored TFLite-micro runtime (``USES tflite_micro``).
 *
 * The NPU "operator" run under ra8_emulator is the tiny deterministic add-constant
 * of the documented ``ra8_npu_sim_cmd.h`` convention (NOT a real Vela command
 * stream, which needs the offline Vela compiler + silicon -- a follow-up). So
 * the quantize -> NPU op -> dequantize pipeline is checkable byte-for-byte in the
 * emulator, while the TFLite-micro `MicroInterpreter` model-driven path is proven
 * to LINK + REGISTER here and exercised end-to-end on silicon later. The verdict
 * is printed over the SCI8 console:
 *
 * @code
 * npu-infer: id=0x10060000 tflm=OK quant=OK irq=OK out=0x........ verdict=PASS
 * @endcode
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

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_device.h"
#include "ra8_err.h"
#include "ra8_ethosu_kernel.h"
#include "ra8_isr.h"
#include "ra8_npu.h"
#include "ra8_npu_quant.h"
#include "ra8_npu_regs.h"
#include "ra8_npu_sim_cmd.h"

/*
 * Compile-time proof that the RA8P1 toolchain selection reached this TU: the NPU
 * driver + TFLite-micro Ethos-U kernel only exist when RA8_HAS_NPU is defined
 * (RA8_DEVICE_RA8P1). Building this app with the RA8D2 toolchain fails loudly here.
 */
#ifndef RA8_HAS_NPU
#error "npu_infer must be built with cmake/toolchain-ra8p1.cmake (RA8_DEVICE_RA8P1)."
#endif

/**
 * @enum npu_infer_size_t
 * @brief Command-stream / tensor-arena sizes and the console baud.
 */
typedef enum : uint32_t {
  k_npu_infer_baud        = 115200U,                /**< SCI8 J-Link OB console baud. */
  k_npu_infer_arena_bytes = 32U,                    /**< Tensor-arena length (bytes). */
  k_npu_infer_cmd_words   = k_ra8_npu_sim_word_num, /**< Command-stream word count.   */
  k_npu_infer_line_cap    = 96U,                    /**< Banner line buffer cap.      */
} npu_infer_size_t;

/**
 * @enum npu_infer_region_t
 * @brief Tensor-region indices the job programs into BASEPn.
 */
typedef enum : uint8_t {
  k_npu_infer_region_weights = 0U, /**< Region 0: Vela weight arena (unused by op).  */
  k_npu_infer_region_input   = 1U, /**< Region 1: op source (quantized input).       */
  k_npu_infer_region_output  = 2U, /**< Region 2: op destination (quantized output). */
  k_npu_infer_region_count   = 3U, /**< Regions programmed (BASEP0..2).              */
} npu_infer_region_t;

/**
 * @enum npu_infer_quant_t
 * @brief Deterministic quantization parameters + the add-constant the op applies.
 */
typedef enum : int32_t {
  k_npu_infer_zero_point = 4,    /**< UINT8 zero-point (non-zero to exercise it). */
  k_npu_infer_addk       = 0x10, /**< Constant the NPU add-const op adds (16).    */
  k_npu_infer_seed_mask  = 15,   /**< Input pattern wrap: in_f[i] uses (i & 15).  */
  k_npu_infer_byte_mask  = 0xFF, /**< 8-bit element wrap (matches the op).        */
} npu_infer_quant_t;

/**
 * @enum npu_infer_fnv_t
 * @brief FNV-1a 32-bit constants for the displayed output checkword.
 */
typedef enum : uint32_t {
  k_npu_infer_fnv_offset = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis. */
  k_npu_infer_fnv_prime  = 0x01000193U, /**< FNV-1a 32-bit prime.        */
} npu_infer_fnv_t;

/**
 * @var s_infer_scale
 * @brief Quantization scale (const float; enums cannot carry a float).
 * @details in_f[i] = scale * (i & 15), so quantize is exact for this fixture.
 * @note Read-only; never modified.
 * @since 0.1.0
 */
static const float s_infer_scale = 0.5F;

/**
 * @var s_infer_deq_tol
 * @brief Absolute tolerance for the dequantized float comparison.
 * @details The fixture is integer-exact, but a small epsilon guards the float
 *          equality against any FPU rounding.
 * @note Read-only; never modified.
 * @since 0.1.0
 */
static const float s_infer_deq_tol = 0.001F;

/**
 * @var s_npu_cmd_stream
 * @brief Sim command stream in SRAM (ra8_npu_sim_cmd.h layout; add-constant op).
 * @details QBASE/QSIZE point the NPU at this; the ra8_emulator model decodes it.
 * @note Not a real Vela program -- see the file header.
 * @since 0.1.0
 */
static uint32_t s_npu_cmd_stream[k_npu_infer_cmd_words];

/**
 * @var s_npu_weights
 * @brief Region 0 weight arena (present by Vela convention; unused by this op).
 * @details Programmed into BASEP0 so the region layout matches a real job.
 * @note Contents irrelevant to the add-constant op.
 * @since 0.1.0
 */
static uint8_t s_npu_weights[k_npu_infer_arena_bytes];

/**
 * @var s_infer_input_f
 * @brief Float input arena, seeded with a deterministic pattern before quantize.
 * @details Quantized into ::s_npu_input by ra8_npu_quantize_u8().
 * @note Seeded by npu_infer_seed().
 * @since 0.1.0
 */
static float s_infer_input_f[k_npu_infer_arena_bytes];

/**
 * @var s_npu_input
 * @brief Region 1 UINT8 input tensor (quantized ::s_infer_input_f).
 * @details The NPU (sim) reads this and writes input+K to the output arena.
 * @note Written by ra8_npu_quantize_u8().
 * @since 0.1.0
 */
static uint8_t s_npu_input[k_npu_infer_arena_bytes];

/**
 * @var s_npu_output
 * @brief Region 2 UINT8 output tensor: zeroed pre-run, holds the NPU result.
 * @details Dequantized into ::s_infer_output_f and verified byte-for-byte.
 * @note Written by the NPU (sim), read back by the app.
 * @since 0.1.0
 */
static uint8_t s_npu_output[k_npu_infer_arena_bytes];

/**
 * @var s_infer_output_f
 * @brief Float output arena, dequantized from ::s_npu_output after the run.
 * @details Checked against scale*(byte - zero_point) for the dequantize path.
 * @note Written by ra8_npu_dequantize_u8().
 * @since 0.1.0
 */
static float s_infer_output_f[k_npu_infer_arena_bytes];

/**
 * @var g_npu_infer_pass
 * @brief Final verdict (1 = PASS, 0 = FAIL), for external (J-Link) inspection.
 * @details `volatile` + non-static so a memprobe can read the verdict.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_npu_infer_pass = 0U;

/**
 * @brief Park the CPU forever in WFI after a fatal init error (a real panic).
 *
 * @details Reserved for failures BEFORE a verdict can be emitted (CGC or SCI8
 *          did not come up), so a ra8_emulator gate scanning for a `*panic_halt`
 *          terminal PC correctly reads this as a failed run.
 *
 * @pre Called only after a fatal init error, before the verdict banner.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void npu_infer_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Park the CPU forever in WFI after the verdict banner (a clean stop).
 *
 * @details Distinct from ::npu_infer_panic_halt: a run that reached the verdict
 *          parks here, whose name deliberately does NOT match the `*panic_halt`
 *          pattern a ra8_emulator gate treats as a give-up.
 *
 * @pre The verdict banner has been emitted over the SCI8 console.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void npu_infer_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up CGC, the SCI8 console, and the ra8_isr substrate. Halts on error.
 *
 * @details The NPU needs no clock beyond the CGC default NPUCLK; the console
 *          (PCLKA/SCICLK) prints the verdict and ra8_isr routes the NPU IRQ.
 *
 * @pre Reset_Handler has initialised .data / .bss.
 * @post On return CGC + SCI8 console + ra8_isr table are ready.
 * @since 0.1.0
 */
static void npu_infer_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    npu_infer_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_npu_infer_baud) != k_ra8_ok) {
    npu_infer_panic_halt();
  }
  if (ra8_isr_init() != k_ra8_ok) {
    npu_infer_panic_halt();
  }
}

/**
 * @brief Fill the command stream (add-constant op) per the sim convention.
 *
 * @pre s_npu_cmd_stream has k_ra8_npu_sim_word_num words.
 * @post s_npu_cmd_stream describes an add-constant of region 1 -> region 2.
 * @since 0.1.0
 */
static void npu_infer_build_stream(void)
{
  s_npu_cmd_stream[k_ra8_npu_sim_word_op] =
    (uint32_t)k_ra8_npu_sim_magic | (uint32_t)k_ra8_npu_sim_op_addk;
  s_npu_cmd_stream[k_ra8_npu_sim_word_src]   = (uint32_t)k_npu_infer_region_input;
  s_npu_cmd_stream[k_ra8_npu_sim_word_dst]   = (uint32_t)k_npu_infer_region_output;
  s_npu_cmd_stream[k_ra8_npu_sim_word_count] = (uint32_t)k_npu_infer_arena_bytes;
  s_npu_cmd_stream[k_ra8_npu_sim_word_const] = (uint32_t)k_npu_infer_addk;
}

/**
 * @brief Seed the float input, quantize it to UINT8, and zero the output arenas.
 *
 * @return `ra8_err_t` from ra8_npu_quantize_u8(), else k_ra8_ok.
 * @retval k_ra8_ok The input arena holds the quantized pattern.
 *
 * @pre The arenas are k_npu_infer_arena_bytes long.
 * @post s_npu_input holds quantize_u8(s_infer_input_f); s_npu_output is zero.
 * @since 0.1.0
 */
static ra8_err_t npu_infer_prepare_input(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_npu_infer_arena_bytes; i++) {
    s_infer_input_f[i] = s_infer_scale * (float)((int32_t)i & (int32_t)k_npu_infer_seed_mask);
    s_npu_output[i]    = 0U;
  }
  return ra8_npu_quantize_u8(s_infer_input_f,
                             s_npu_input,
                             (size_t)k_npu_infer_arena_bytes,
                             s_infer_scale,
                             (int32_t)k_npu_infer_zero_point);
}

/**
 * @brief Submit the job, arm the IRQ latch, kick it, and await the NPU interrupt.
 *
 * @return `ra8_err_t` from the first failing driver call, else k_ra8_ok.
 * @retval k_ra8_ok Job completed via the interrupt path (STATUS.cmd_end latched).
 *
 * @pre The command stream + input arena are populated and the NPU IRQ is routed.
 * @post On k_ra8_ok the output arena holds the NPU result.
 * @since 0.1.0
 */
static ra8_err_t npu_infer_run_job_irq(void)
{
  ra8_npu_job_t job                           = {};
  job.cmd_stream                              = s_npu_cmd_stream;
  job.cmd_stream_bytes                        = (uint32_t)sizeof(s_npu_cmd_stream);
  job.region_count                            = (uint8_t)k_npu_infer_region_count;
  job.region_base[k_npu_infer_region_weights] = (uint64_t)(uintptr_t)s_npu_weights;
  job.region_base[k_npu_infer_region_input]   = (uint64_t)(uintptr_t)s_npu_input;
  job.region_base[k_npu_infer_region_output]  = (uint64_t)(uintptr_t)s_npu_output;

  const ra8_err_t sub = ra8_npu_submit(&job);
  if (sub != k_ra8_ok) {
    return sub;
  }
  const ra8_err_t arm = ra8_npu_irq_arm();
  if (arm != k_ra8_ok) {
    return arm;
  }
  const ra8_err_t run = ra8_npu_run();
  if (run != k_ra8_ok) {
    return run;
  }
  return ra8_npu_wait_irq();
}

/**
 * @brief Verify quantize + NPU op + dequantize, folding the output to a checkword.
 *
 * @param[out] out_check FNV-1a digest of the output bytes (for the banner).
 * @return true when every stage matches its deterministic expectation.
 *
 * @pre out_check is non-NULL; the job has completed and been dequantized.
 * @post *out_check holds the output digest regardless of the verdict.
 * @since 0.1.0
 */
static bool npu_infer_verify(uint32_t* out_check)
{
  uint32_t check = (uint32_t)k_npu_infer_fnv_offset;
  bool     ok    = true;
  for (uint32_t i = 0U; i < (uint32_t)k_npu_infer_arena_bytes; i++) {
    const int32_t expect_q =
      ((int32_t)i & (int32_t)k_npu_infer_seed_mask) + (int32_t)k_npu_infer_zero_point;
    const int32_t expect_out =
      (expect_q + (int32_t)k_npu_infer_addk) & (int32_t)k_npu_infer_byte_mask;
    const float expect_f =
      s_infer_scale * (float)((int32_t)s_npu_output[i] - (int32_t)k_npu_infer_zero_point);
    const float diff = (s_infer_output_f[i] > expect_f) ? (s_infer_output_f[i] - expect_f)
                                                        : (expect_f - s_infer_output_f[i]);
    if ((int32_t)s_npu_input[i] != expect_q) {
      ok = false; /* quantize stage */
    }
    if ((int32_t)s_npu_output[i] != expect_out) {
      ok = false; /* NPU op stage (IRQ completion) */
    }
    if (diff > s_infer_deq_tol) {
      ok = false; /* dequantize stage */
    }
    check = (check ^ (uint32_t)s_npu_output[i]) * (uint32_t)k_npu_infer_fnv_prime;
  }
  *out_check = check;
  return ok;
}

/**
 * @brief Format and print the one-line verdict banner over the SCI8 console.
 *
 * @param[in] id      NPU_ID read after init.
 * @param[in] tflm_ok Whether the real TFLite-micro Ethos-U op is registered.
 * @param[in] run_ok  Whether the submit/arm/run/wait-irq sequence returned ok.
 * @param[in] check   Output checkword from npu_infer_verify().
 * @param[in] pass    Final verdict (all stages matched).
 *
 * @pre The SCI8 console is initialised.
 * @post One banner line has been written and flushed to the console.
 * @since 0.1.0
 */
static void npu_infer_emit(uint32_t id, bool tflm_ok, bool run_ok, uint32_t check, bool pass)
{
  char      line[k_npu_infer_line_cap];
  const int n = snprintf(line,
                         sizeof(line),
                         "npu-infer: id=0x%08X tflm=%s irq=%s out=0x%08X verdict=%s\r\n",
                         (unsigned)id,
                         tflm_ok ? "OK" : "FAIL",
                         run_ok ? "OK" : "FAIL",
                         (unsigned)check,
                         pass ? "PASS" : "FAIL");
  if (n > 0) {
    (void)ra8_board_uart_console_write((const uint8_t*)line, (size_t)n);
    (void)ra8_board_uart_console_flush();
  }
}

/**
 * @brief Bring up the NPU, register its completion IRQ, and run one inference.
 *
 * @param[out] out_id    NPU_ID captured after init.
 * @param[out] out_tflm  Whether the real TFLite-micro Ethos-U op is registered.
 * @param[out] out_check Output checkword (valid only when the return is true).
 * @return true when init + IRQ registration + the quantize/op/dequantize pipeline
 *         all succeeded and every stage matched its expectation.
 *
 * @pre The console + ra8_isr substrate are up (npu_infer_setup_or_halt ran).
 * @pre out_id / out_tflm / out_check are non-NULL.
 * @post *out_id / *out_tflm / *out_check reflect the run.
 * @since 0.1.0
 */
static bool npu_infer_execute(uint32_t* out_id, bool* out_tflm, uint32_t* out_check)
{
  *out_id    = 0U;
  *out_tflm  = (ra8_ethosu_kernel_available() == 1);
  *out_check = 0U;

  if (ra8_npu_init() != k_ra8_ok) {
    return false;
  }
  (void)ra8_npu_read_id(out_id);

  const ra8_err_t reg = ra8_isr_register((ra8_elc_event_t)k_ra8_npu_event_irq,
                                         ra8_npu_irq_handler,
                                         nullptr,
                                         (uint8_t)k_ra8_isr_prio_default,
                                         nullptr);
  if (reg != k_ra8_ok) {
    return false;
  }
  ra8_isr_globals_enable();

  npu_infer_build_stream();
  if (npu_infer_prepare_input() != k_ra8_ok) {
    return false;
  }
  if (npu_infer_run_job_irq() != k_ra8_ok) {
    return false;
  }
  if (ra8_npu_dequantize_u8(s_npu_output,
                            s_infer_output_f,
                            (size_t)k_npu_infer_arena_bytes,
                            s_infer_scale,
                            (int32_t)k_npu_infer_zero_point) != k_ra8_ok) {
    return false;
  }
  return npu_infer_verify(out_check);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: quantize + NPU op (IRQ) + dequantize, print verdict.
 *
 * @return Never returns (halts in WFI after emitting the banner).
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post The verdict banner is on the console; the CPU is parked in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  npu_infer_setup_or_halt();

  uint32_t id    = 0U;
  bool     tflm  = false;
  uint32_t check = 0U;
  /* run_ok = init + IRQ-driven quantize/op/dequantize pipeline all matched. The
   * TFLite-micro registration (tflm) is a separate leg reported on its own field;
   * the verdict is PASS only when every leg holds. */
  const bool run_ok = npu_infer_execute(&id, &tflm, &check);
  const bool pass   = run_ok && tflm && (id != 0U);

  g_npu_infer_pass = pass ? 1U : 0U;
  npu_infer_emit(id, tflm, run_ok, check, pass);

  npu_infer_park();
  return 0;
}
#pragma GCC diagnostic pop
