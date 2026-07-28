/**
 * @file examples/ek_ra8d2/hw_validated/hil/dma_coherency_hil/main.c
 * @brief DMAC mem-to-mem coherency HIL proof with the M85 D-cache ENABLED
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Single-core (Cortex-M85) hardware-in-the-loop test that proves the
 * DMA cache-maintenance pattern -- **clean-before / invalidate-after**
 * -- keeps a DMAC0 mem-to-mem copy coherent with the M85 L1 D-cache
 * while the cache is turned ON.
 *
 * This app is the first in the tree to build with
 * ``RA8_BOOT_ENABLE_CACHE_MPU`` (set in its CMakeLists.txt). With that
 * flag the shared boot (``libs/ra8_board_ek_ra8d2/boot/system_init.c``)
 * brings up the MPU (5 regions) + I-cache + D-cache + branch predictor.
 * Both buffers live in MPU **region 1** (0x22000000, M85-private
 * **cacheable** SRAM), so the cache hazard is real on silicon.
 *
 * Sequence (one-shot, then WFI):
 *   1. ``ra8_cgc_init()`` + ``ra8_mstp_init()`` + SCI8 J-Link OB console.
 *   2. Fill ``s_src`` with a deterministic pattern. These are M85
 *      stores, so the fresh bytes sit **dirty in the D-cache**, not yet
 *      in SRAM. ``s_dst`` is filled with a sentinel so a no-op copy
 *      fails the verify.
 *   3. ``ra8_cache_dcache_clean_by_addr(s_src, sizeof s_src)`` -- write
 *      the dirty source lines back so the DMAC reads the real pattern
 *      from memory, not stale SRAM (**clean-before**).
 *   4. Programme DMAC0 channel 0 for a 32-bit-wide, increment-both,
 *      single-block transfer of 256 words and software-trigger it
 *      (``DMREQ.SWREQ``); poll ``DMSTS.ACT`` to completion.
 *   5. ``ra8_cache_dcache_invalidate_by_addr(s_dst, sizeof s_dst)`` --
 *      drop the stale destination lines so the verify reads what the
 *      DMAC wrote to memory, not a cached copy (**invalidate-after**).
 *   6. Verify ``s_dst == s_src`` word-for-word. PASS -> emit
 *      ``"dma_coherency_hil: dma coherent PASS\r\n"`` over VCOM and the
 *      same result over ITM (``ra8_log_info``), toggle LED1. Mismatch ->
 *      FAIL banner + LED2. Either way the function parks in WFI so the
 *      emulator's idle-stop terminates the run after the one-shot.
 *
 * @par ra8_emulator note (off-target path)
 * ``tools/ra8_emulator`` DOES model the DMAC mem-to-mem transfer:
 * ``board_periph_dmac.c`` (``dmac_copy_units``) actually moves the bytes
 * in emulated memory on the ``DMREQ.SWREQ`` software trigger, so the
 * **real** ``ra8_dmac`` path runs off-target -- no CPU-memcpy fallback is
 * needed. ra8_emulator's memory is byte-exact and it does **not** model the
 * L1 D-cache, so the clean/invalidate calls are exercised (the line-size
 * and barrier logic run) but have no caching effect, and the copy
 * verifies trivially. The cache hazard this app guards against is only
 * observable on real silicon. ``RA8_OFF_TARGET`` is a host-unit-test
 * define and is NOT set for the ARM cross-build that ra8_emulator executes,
 * so there is nothing to ``#ifdef`` here.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cache.h"
#include "ra8_cgc.h"
#include "ra8_dmac.h"
#include "ra8_dmac_regs.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_dma_coh_baud       = 115200U,     /**< J-Link OB VCOM baud rate.           */
  k_dma_coh_buf_words  = 256U,        /**< Words per buffer (256 x 4 = 1 KiB). */
  k_dma_coh_poll_limit = 100000U,     /**< DMSTS.ACT poll bound.               */
  k_dma_coh_sentinel   = 0xA5A5A5A5U, /**< Pre-fill so a no-op copy fails.     */
} dma_coh_config_t;

/** @brief Single-byte constants. */
typedef enum : uint8_t {
  k_dma_coh_channel = 0U,  /**< DMAC0 channel index.       */
  k_dma_coh_byte_sh = 8U,  /**< Pattern xor shift.         */
  k_dma_coh_align   = 32U, /**< D-cache line size (bytes). */
} dma_coh_byte_t;

/** @brief VCOM success banner. Unique; not a substring of the FAIL line. */
static const uint8_t k_dma_coh_pass_banner[] = "dma_coherency_hil: dma coherent PASS\r\n";

/** @brief VCOM failure banner (caught by hil.conf HIL_EXPECT_NEGATIVE). */
static const uint8_t k_dma_coh_fail_banner[] = "dma_coherency_hil: dma coherent FAIL\r\n";

/**
 * @brief Source buffer -- M85 writes leave it dirty in the D-cache.
 *
 * @details 1 KiB, aligned to a 32-byte D-cache line and an exact whole
 *          number of lines, so ``ra8_cache_dcache_clean_by_addr`` cleans
 *          exactly the buffer with no partial-line spill.
 *
 * @note Lives in MPU region 1 (cacheable SRAM) when RA8_BOOT_ENABLE_CACHE_MPU.
 * @warning Do not read after a DMAC write without invalidating first.
 * @since 0.1.0
 */
alignas(k_dma_coh_align) static uint32_t s_src[k_dma_coh_buf_words];

/**
 * @brief Destination buffer the DMAC writes; M85 must invalidate to read.
 *
 * @details 1 KiB, 32-byte aligned and a whole number of cache lines so
 *          ``ra8_cache_dcache_invalidate_by_addr`` drops exactly the
 *          buffer's lines.
 *
 * @note Lives in MPU region 1 (cacheable SRAM) when RA8_BOOT_ENABLE_CACHE_MPU.
 * @warning Stale cached lines here are the exact hazard this app proves.
 * @since 0.1.0
 */
alignas(k_dma_coh_align) static uint32_t s_dst[k_dma_coh_buf_words];

/**
 * @brief Halt forever in WFI -- panic stop on a fatal init failure.
 *
 * @details Parks the core; only a debugger or external reset wakes it.
 *          Under ra8_emulator a WFI with no pending work triggers
 *          RA8_EMU_IDLE_STOP, terminating the run cleanly.
 *
 * @pre Called only after a fatal error in boot or after the one-shot test.
 * @pre IRQs are in whatever state the caller left them.
 * @post The core never returns from this function.
 * @post No further bus activity is generated.
 *
 * @note Not thread-safe; terminal sink.
 * @since 0.1.0
 */
[[noreturn]] static void dma_coh_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + MSTP + SCI8 console + LEDs up. Panic-halts on fail.
 *
 * @details The DMAC0 module clock is ungated by ``ra8_mstp_init()``
 *          (``ra8_dmac_start`` references the same MSTP gate). The SCI8
 *          J-Link OB console (TXD8 = PD_02 / RXD8 = PD_03 @ 115200 8N1)
 *          comes up through the BSP console API.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has enabled the MPU + caches (RA8_BOOT_ENABLE_CACHE_MPU).
 * @post On success CGC, MSTP, the console and both LEDs are initialised.
 * @post On any failure the function does not return (parks in WFI).
 *
 * @note Single-threaded init context; not thread-safe.
 * @since 0.1.0
 */
static void dma_coh_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    dma_coh_park();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    dma_coh_park();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dma_coh_baud) != k_ra8_ok) {
    dma_coh_park();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dma_coh_park();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    dma_coh_park();
  }
}

/**
 * @brief Fill ``s_src`` with a deterministic pattern; prime ``s_dst``.
 *
 * @details ``s_src[i] = i ^ (i >> 8)`` are M85 stores that sit dirty in
 *          the D-cache. ``s_dst`` is primed with a sentinel distinct from
 *          every source word so a copy that never lands fails the verify.
 *
 * @par MC/DC:
 * Single counting loop, no compound decision; only the implicit loop
 * bound. No N+1 vectors required.
 *
 * @pre Buffers are statically allocated and 32-byte aligned.
 * @pre Called single-threaded before the DMAC is armed.
 * @post Every word of ``s_src`` holds the pattern.
 * @post Every word of ``s_dst`` holds ``k_dma_coh_sentinel``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void dma_coh_fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dma_coh_buf_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_dma_coh_byte_sh);
    s_dst[i] = (uint32_t)k_dma_coh_sentinel;
  }
}

/**
 * @brief Verify ``s_dst`` matches ``s_src`` element-by-element.
 *
 * @details Must run AFTER ``ra8_cache_dcache_invalidate_by_addr(s_dst)``
 *          so the reads miss the cache and fetch the DMAC-written bytes
 *          from SRAM.
 *
 * @par MC/DC:
 * One atomic condition ``s_dst[i] != s_src[i]`` x 2 vectors: all-match
 * (steady state, this run) and one mismatch (the cache-hazard regression
 * exercised on silicon when the invalidate is omitted).
 *
 * @return 1 if all words equal, 0 otherwise.
 * @retval 1 Buffers identical -- copy + maintenance coherent.
 * @retval 0 At least one word differs.
 *
 * @pre Both buffers are filled and the DMAC copy has completed.
 * @pre ``s_dst`` lines have been invalidated.
 * @post The return value is exactly 0 or 1.
 * @post Neither buffer is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static uint8_t dma_coh_verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dma_coh_buf_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Clean src, run the DMAC block copy, then invalidate dst.
 *
 * @details Implements the clean-before / invalidate-after pattern:
 *   1. ``ra8_cache_dcache_clean_by_addr(s_src)`` flushes the dirty source
 *      lines so the DMAC reads the real pattern from SRAM
 *      (HUM-independent Arm v8-M cache maintenance).
 *   2. ``ra8_dmac_start_block`` programmes DMAC0 channel 0 in block mode
 *      (DMTMD.MD = 10b, HUM Ch 17.2.10 p 738): one software trigger moves
 *      the whole ``k_dma_coh_buf_words``-word block, 32-bit units,
 *      increment both sides.
 *   3. ``DMREQ.SWREQ`` software-triggers the block (HUM Ch 17.2.15 p 744).
 *   4. Poll ``DMSTS.ACT`` (HUM Ch 17.2.16 p 745) until the controller
 *      goes idle -- the completion gate for a synchronous block copy.
 *   5. ``ra8_cache_dcache_invalidate_by_addr(s_dst)`` drops the stale
 *      destination lines so the subsequent verify reads from SRAM.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Copy completed and both maintenance ops ran.
 * @retval k_ra8_err_hw_error  A cache op failed or the channel accessor
 *                            returned NULL.
 * @retval k_ra8_err_hw_timeout DMSTS.ACT never cleared within the poll bound.
 * @retval (other)            Propagated ``ra8_dmac_start_block`` /
 *                            ``ra8_dmac_stop`` error.
 *
 * @pre ``ra8_mstp_init()`` has run and the buffers are filled.
 * @pre IRQs are enabled but the channel uses no completion ISR here.
 * @post On ``k_ra8_ok`` the DMAC has copied ``s_src`` into SRAM-backed
 *       ``s_dst`` and ``s_dst``'s cache lines are invalid.
 * @post On any error the channel is stopped (DMCNT.DTE = 0).
 *
 * @note Not thread-safe; single-shot init-context use.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dma_coh_run_copy(void)
{
  /* clean-before: dirty src lines -> SRAM so the DMAC reads fresh bytes. */
  if (ra8_cache_dcache_clean_by_addr(s_src, (uint32_t)sizeof(s_src)) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }

  const ra8_dmac_config_t cfg = {
    .src         = (uint32_t)(uintptr_t)s_src,
    .dst         = (uint32_t)(uintptr_t)s_dst,
    .count       = (uint16_t)k_dma_coh_buf_words,
    .block_count = 1U,
    .width       = k_ra8_dmac_width_word,
    .src_inc     = true,
    .dst_inc     = true,
  };
  ra8_err_t err = ra8_dmac_start_block((uint8_t)k_dma_coh_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_dma_coh_channel);
  if (reg == nullptr) {
    (void)ra8_dmac_stop((uint8_t)k_dma_coh_channel);
    return k_ra8_err_hw_error;
  }

  /* Software trigger: assert DMREQ.SWREQ to request the block transfer.
   * HUM Ch 17.2.15 p 744: write SWREQ=1; the bit auto-clears when the
   * controller accepts the request (CLRS=0 is the reset default). */
  reg->DMREQ = (uint8_t)k_ra8_dmreq_swreq_mask;

  /* Poll DMSTS.ACT until the controller goes idle.
   * HUM Ch 17.2.16 p 745: ACT clears when the transfer completes. */
  bool done = false;
  for (uint32_t i = 0U; i < (uint32_t)k_dma_coh_poll_limit; ++i) {
    if ((reg->DMSTS & (uint8_t)k_ra8_dmsts_act_mask) == 0U) {
      done = true;
      break;
    }
  }
  err = ra8_dmac_stop((uint8_t)k_dma_coh_channel);
  if (!done) {
    return k_ra8_err_hw_timeout;
  }
  if (err != k_ra8_ok) {
    return err;
  }

  /* invalidate-after: drop stale dst lines so the verify reads SRAM. */
  if (ra8_cache_dcache_invalidate_by_addr(s_dst, (uint32_t)sizeof(s_dst)) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Emit the PASS/FAIL result over VCOM and ITM, toggle an LED.
 *
 * @param[in] ok Non-zero if the coherency check passed.
 *
 * @pre The console has been initialised by ``dma_coh_setup_or_halt``.
 * @pre Both LEDs have been initialised.
 * @post Exactly one terminal banner has been written and flushed to VCOM.
 * @post The matching result line has been emitted over ITM.
 *
 * @note Not thread-safe; single-shot reporting.
 * @since 0.1.0
 */
static void dma_coh_report(uint8_t ok)
{
  if (ok != 0U) {
    (void)ra8_board_uart_console_write(k_dma_coh_pass_banner,
                                       (size_t)(sizeof(k_dma_coh_pass_banner) - 1U));
    (void)ra8_board_uart_console_flush();
    ra8_log_info("dma_coherency_hil", "dma coherent PASS");
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  } else {
    (void)ra8_board_uart_console_write(k_dma_coh_fail_banner,
                                       (size_t)(sizeof(k_dma_coh_fail_banner) - 1U));
    (void)ra8_board_uart_console_flush();
    ra8_log_info("dma_coherency_hil", "dma coherent FAIL");
    (void)ra8_board_led_toggle(k_ra8_board_led2);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: one-shot DMAC coherency proof, then WFI.
 *
 * @return Never returns (parks in WFI after reporting).
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit enabled the MPU + caches via RA8_BOOT_ENABLE_CACHE_MPU.
 * @post Exactly one PASS/FAIL banner is emitted, then the core parks.
 * @post The core ends in WFI so ra8_emulator's idle-stop terminates the run.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  dma_coh_setup_or_halt();
  ra8_isr_globals_enable();

  dma_coh_fill_buffers();
  const ra8_err_t err = dma_coh_run_copy();
  const uint8_t   ok  = (err == k_ra8_ok && dma_coh_verify() != 0U) ? 1U : 0U;
  dma_coh_report(ok);

  dma_coh_park();
  return 0;
}
#pragma GCC diagnostic pop
