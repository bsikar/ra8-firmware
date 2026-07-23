/**
 * @file examples/ek_ra8d2/hw_pending/ra8_ftl_demo/main.c
 * @brief Wear-levelled block I/O + power-cycle survival over the FTL (#258).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates the Flash Translation Layer (`ra8_ftl.h`) end to end over the
 * RA8D2's on-chip extra MRAM -- a non-volatile, erase-before-write medium. The
 * FTL turns that erase-before-write device into a clean free-overwrite block
 * device and spreads wear by relocating each logical-block write to a fresh,
 * least-worn physical block (copy-on-write).
 *
 * The extra-MRAM window is 24 logical blocks (12 KiB). This demo hands the FTL
 * the first 23 physical blocks (presenting 8 logical blocks, so 15 spare for
 * relocation headroom) and reserves the last physical block as a non-volatile
 * slot for the FTL mapping checkpoint.
 *
 * The run is three acts:
 *
 *   1. **Wear-levelling** -- write ONE logical block repeatedly and, after each
 *      write, query ::ra8_ftl_phys_of to show the backing physical block index
 *      migrating while the logical address stays fixed. Each write is read back
 *      and byte-verified.
 *   2. **Checkpoint** -- serialise the FTL mapping (::ra8_ftl_checkpoint_save)
 *      and store the blob in the reserved MRAM block (the FTL keeps no on-media
 *      metadata of its own, so this is what lets the mapping survive a reset).
 *   3. **Power-cycle survival** -- model a reset: discard the FTL handle and its
 *      caller tables (SRAM is volatile) while the MRAM retains its bytes. A
 *      naive re-init has lost the mapping (the logical block reads back the
 *      erase value), so ::ra8_ftl_checkpoint_load reloads the checkpoint from
 *      MRAM and the data -- and the exact physical mapping -- reappears.
 *
 * board_sim models the MACI program/erase sequence (board_periph_mram.c), so
 * the whole run is observable headless over the SCI8 console. A successful run
 * ends with:
 * `ra8_ftl_demo: wear-level + power-cycle-survive on extra MRAM PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_ftl.h"
#include "ra8_io.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/** @enum demo_addr_t @brief Extra-MRAM window base address.
 *  @warning One-time-programmable OTP option-setting memory (HUM Ch 59.7.4.5
 *  Table 59.15 p 3592), not rewritable data-flash: the wear-level erase/rewrite
 *  cycle cannot work on real silicon. A rewritable-medium retarget is #315. */
typedef enum : uintptr_t {
  k_demo_mram_base = k_ra8_flash_extra_start, /**< Extra-MRAM OTP window base (0x02E07600). */
} demo_addr_t;

/** @enum demo_const_t @brief Console + FTL geometry + workload knobs. */
typedef enum : uint32_t {
  k_demo_uart_chan    = 8U,      /**< SCI8 J-Link OB console.                */
  k_demo_uart_baud    = 115200U, /**< Console baud.                          */
  k_demo_total_blocks = 24U,     /**< Whole extra MRAM (12 KiB).             */
  k_demo_ftl_phys     = 23U,     /**< Physical blocks handed to the FTL.     */
  k_demo_ftl_logical  = 8U,      /**< Logical blocks the FTL presents.       */
  k_demo_meta_lba     = 23U,     /**< Reserved raw block for the checkpoint. */
  k_demo_one_block    = 1U,      /**< Single-block transfer count.           */
  k_demo_test_lbn     = 2U,      /**< Logical block written repeatedly.      */
  k_demo_writes       = 12U,     /**< Repeated overwrites of the test block. */
  k_demo_block        = 512U,    /**< Bytes per block.                       */
  k_demo_pin_shift    = 8U,      /**< Port byte position in ra8_port_pin_t.  */
  k_demo_mrcfreq_mhz  = 200U,    /**< Code-MRAM advertised clock (MHz).      */
  k_demo_mrefreq_mhz  = 100U,    /**< Extra-MRAM advertised clock (MHz).     */
  k_demo_seed_mul     = 17U,     /**< Test-pattern index multiplier.         */
  k_demo_lbn_mul      = 5U,      /**< Test-pattern logical-block bias.       */
  k_demo_tag_base     = 1U,      /**< First generation tag.                  */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra8_port_pin_t k_demo_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra8_port_pin_t k_demo_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_3);

/** @brief Raw MRAM block-device handle + its backend state. */
static ra8_io_blockdev_t            s_bd;
static ra8_io_blockdev_mram_state_t s_mstate;
/** @brief FTL handle + the presented free-overwrite block device. */
static ra8_ftl_t         s_ftl;
static ra8_io_blockdev_t s_present;
/** @brief FTL caller storage: map, per-physical metadata, copy scratch. */
static uint16_t         s_map[(size_t)k_demo_ftl_logical];
static ra8_ftl_pblock_t s_pblocks[(size_t)k_demo_ftl_phys];
static uint8_t          s_scratch[(size_t)k_demo_block];
/** @brief One-block checkpoint buffer (the reserved MRAM slot holds this). */
static uint8_t s_ckbuf[(size_t)k_demo_block];
/** @brief UART output stream + its sink state. */
static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra8_ftl_demo";

/**
 * @brief Print a NUL-terminated string on the demo's UART stream.
 *
 * @details Thin wrapper over ::ra8_io_stream_puts bound to ::s_uart.
 *
 * @param[in] msg NUL-terminated ASCII string (CR/LF supplied by the caller).
 *
 * @return None.
 *
 * @pre ::s_uart was initialised by ::ra8_io_stream_uart_init.
 * @pre @p msg is non-NULL and NUL-terminated.
 * @post The bytes of @p msg are queued on the UART stream.
 * @post No other state changes.
 *
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
static void demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Print `logical L now at physical P` for one relocation observation.
 *
 * @details Composes a diagnostic line from three stream calls so the migrating
 *          physical index is visible per write without a printf dependency.
 *
 * @param[in] lbn  Logical block number.
 * @param[in] phys Physical block index currently backing @p lbn.
 *
 * @return None.
 *
 * @pre ::s_uart was initialised.
 * @pre @p phys is a valid physical index (`< k_demo_ftl_phys`).
 * @post One diagnostic line is queued on the UART stream.
 * @post No other state changes.
 *
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
static void demo_report_map(uint32_t lbn, uint16_t phys)
{
  demo_print("ra8_ftl_demo: logical ");
  (void)ra8_io_stream_put_u32(&s_uart, lbn);
  demo_print(" now at physical ");
  (void)ra8_io_stream_put_u32(&s_uart, (uint32_t)phys);
  demo_print("\r\n");
}

/**
 * @brief Bring up CGC + SysTick + the SCI8 console; halt forever on any failure.
 *
 * @details Initialises clocks, reads CPU/PCLKA rates, starts the time base,
 *          routes the SCI8 console pins, and opens the SCI8 UART.
 *
 * @return None.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre Runs single-threaded during early boot.
 * @post On success SCI8 is open at ::k_demo_uart_baud and the time base runs.
 * @post On any failure the function never returns (infinite halt loop).
 *
 * @note Not thread-safe; boot-context only.
 * @since 0.1.0
 */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_demo_txd, k_ra8_psel_sci_async, "demo.txd") != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_demo_rxd, k_ra8_psel_sci_async, "demo.rxd") != k_ra8_ok)) {
    while (true) {
    }
  }
  const ra8_sci_cfg_t sci_cfg = {.baud      = (uint32_t)k_demo_uart_baud,
                                 .data_bits = k_ra8_sci_data_8,
                                 .parity    = k_ra8_sci_parity_none,
                                 .stop_bits = k_ra8_sci_stop_1,
                                 .pclk_hz   = pclka_hz};
  if (ra8_sci_init((uint8_t)k_demo_uart_chan, &sci_cfg) != k_ra8_ok) {
    while (true) {
    }
  }
}

/**
 * @brief Bring up the MRAM controller and bind the raw MRAM block device.
 *
 * @details Initialises the MRAM controller (`ra8_flash_init`) and binds an
 *          erase-before-write block device over the whole 24-block extra-MRAM
 *          window; the FTL manages the first 23 blocks and the demo owns block
 *          23 for the checkpoint.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The raw MRAM block device ::s_bd is ready.
 * @retval (other) The first failing bring-up step's error code.
 *
 * @pre ::demo_setup_or_halt has run (clocks + console up).
 * @pre The extra-MRAM region is present and writable.
 * @post On success ::s_bd reads/writes/erases the fenced MRAM window.
 * @post On any non-ok return ::s_bd is left unbound.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_open(void)
{
  const ra8_flash_cfg_t fcfg = {.mrcfreq_mhz        = (uint16_t)k_demo_mrcfreq_mhz,
                                .mrefreq_mhz        = (uint8_t)k_demo_mrefreq_mhz,
                                .prefetch_en        = true,
                                .ecc_encoder_enable = true,
                                .ecc_decoder_enable = true};
  RA8_RETURN_ON_ERROR(ra8_flash_init(&fcfg), s_tag, "flash init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_mram_init(&s_bd,
                                                &s_mstate,
                                                (uintptr_t)k_demo_mram_base,
                                                (uint32_t)k_demo_total_blocks,
                                                false),
                      s_tag,
                      "mram blockdev init");
  return k_ra8_ok;
}

/**
 * @brief Write one generation of the test pattern and read it back verified.
 *
 * @details Fills a block with a deterministic pattern keyed by @p lbn and
 *          @p tag, writes it through the FTL-presented device (copy-on-write
 *          relocation), reads it back and byte-compares, then reports the new
 *          physical mapping through ::ra8_ftl_phys_of.
 *
 * @param[in]  lbn      Logical block number to write.
 * @param[in]  tag      Generation tag distinguishing successive overwrites.
 * @param[out] out_phys Receives the physical block now backing @p lbn.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The block round-tripped and mapped.
 * @retval k_ra8_err_null_ptr          @p out_phys was NULL.
 * @retval k_ra8_err_checksum_mismatch The read-back bytes differed.
 * @retval (other)                    The first failing FTL / device call's code.
 *
 * @pre The FTL device ::s_present is bound.
 * @pre @p lbn < ::k_demo_ftl_logical.
 * @post On success `*out_phys` is the live physical index for @p lbn.
 * @post No resource is leaked on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_write_verify(uint32_t lbn, uint32_t tag, uint16_t* out_phys)
{
  RA8_CHECK_NULL_PTR(out_phys, s_tag, "out_phys must not be nullptr");
  uint8_t blk[(size_t)k_demo_block];
  for (uint32_t i = 0; i < (uint32_t)k_demo_block; ++i) {
    blk[i] = (uint8_t)((i * (uint32_t)k_demo_seed_mul) + (lbn * (uint32_t)k_demo_lbn_mul) + tag);
  }
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_write(&s_present, lbn, (uint32_t)k_demo_one_block, blk),
                      s_tag,
                      "write");
  uint8_t got[(size_t)k_demo_block];
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(&s_present, lbn, (uint32_t)k_demo_one_block, got),
                      s_tag,
                      "read");
  if (memcmp(got, blk, sizeof(blk)) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  return ra8_ftl_phys_of(&s_ftl, lbn, out_phys);
}

/**
 * @brief Act 1: init the FTL and hammer one logical block to show migration.
 *
 * @details Initialises the FTL over the raw MRAM device (23 physical blocks,
 *          8 logical presented), binds the presented device, then overwrites
 *          ::k_demo_test_lbn ::k_demo_writes times. Each write is verified and
 *          its new physical index reported; the count of physical relocations
 *          and the final physical index are returned.
 *
 * @param[out] migrations Receives the number of physical relocations observed.
 * @param[out] last_phys  Receives the final physical index of the test block.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok        All writes round-tripped and the block relocated.
 * @retval k_ra8_err_null_ptr @p migrations or @p last_phys was NULL.
 * @retval (other)         The first failing init / write / verify call's code.
 *
 * @pre ::demo_open returned k_ra8_ok; ::s_bd is bound.
 * @pre @p migrations and @p last_phys are writable.
 * @post On success ::s_present is bound and the test block is live.
 * @post `*migrations >= 1` whenever spare blocks allow relocation.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_wear_phase(uint32_t* migrations, uint16_t* last_phys)
{
  RA8_CHECK_NULL_PTR(migrations, s_tag, "migrations must not be nullptr");
  RA8_CHECK_NULL_PTR(last_phys, s_tag, "last_phys must not be nullptr");
  RA8_RETURN_ON_ERROR(ra8_ftl_init(&s_ftl,
                                   &s_bd,
                                   s_map,
                                   (uint32_t)k_demo_ftl_logical,
                                   s_pblocks,
                                   (uint32_t)k_demo_ftl_phys,
                                   s_scratch),
                      s_tag,
                      "ftl init");
  RA8_RETURN_ON_ERROR(ra8_ftl_as_blockdev(&s_ftl, &s_present), s_tag, "ftl as blockdev");
  uint32_t moves = 0U;
  uint16_t prev  = (uint16_t)k_ra8_ftl_unmapped;
  for (uint32_t k = 0; k < (uint32_t)k_demo_writes; ++k) {
    uint16_t        cur = 0U;
    const ra8_err_t e =
      demo_write_verify((uint32_t)k_demo_test_lbn, (uint32_t)k_demo_tag_base + k, &cur);
    if (e != k_ra8_ok) {
      return e;
    }
    demo_report_map((uint32_t)k_demo_test_lbn, cur);
    if ((prev != (uint16_t)k_ra8_ftl_unmapped) && (cur != prev)) {
      moves += 1U;
    }
    prev = cur;
  }
  *migrations = moves;
  *last_phys  = prev;
  return k_ra8_ok;
}

/**
 * @brief Act 2: serialise the FTL mapping into the reserved MRAM block.
 *
 * @details Confirms the checkpoint fits one 512-byte block, serialises it with
 *          ::ra8_ftl_checkpoint_save, erases the reserved raw block, and
 *          programs the checkpoint into it -- the persistent metadata that lets
 *          the mapping outlive the volatile SRAM tables.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The checkpoint is stored in MRAM.
 * @retval k_ra8_err_invalid_size The checkpoint does not fit one block.
 * @retval (other)               The first failing save / erase / write code.
 *
 * @pre ::demo_wear_phase returned k_ra8_ok; ::s_ftl is initialised.
 * @pre ::k_demo_meta_lba is outside the FTL's physical range.
 * @post On success raw block ::k_demo_meta_lba holds a loadable checkpoint.
 * @post No FTL mapping state is mutated.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_checkpoint(void)
{
  uint32_t need = 0U;
  RA8_RETURN_ON_ERROR(ra8_ftl_checkpoint_size(&s_ftl, &need), s_tag, "checkpoint size");
  if (need > (uint32_t)k_demo_block) {
    return k_ra8_err_invalid_size;
  }
  RA8_RETURN_ON_ERROR(ra8_ftl_checkpoint_save(&s_ftl, s_ckbuf, (uint32_t)k_demo_block),
                      s_tag,
                      "checkpoint save");
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_erase(&s_bd, (uint32_t)k_demo_meta_lba, (uint32_t)k_demo_one_block),
    s_tag,
    "meta erase");
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_write(&s_bd, (uint32_t)k_demo_meta_lba, (uint32_t)k_demo_one_block, s_ckbuf),
    s_tag,
    "meta write");
  return k_ra8_ok;
}

/**
 * @brief Act 3a: model a reset and prove the naive re-open lost the mapping.
 *
 * @details Zeroes the FTL handle and its caller tables (volatile SRAM lost)
 *          while the MRAM device state is retained, re-initialises the FTL over
 *          the retained backing store, and reads the test block back: with the
 *          mapping gone it must read the erase value, confirming the checkpoint
 *          is required for survival.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The mapping was lost as expected (all erase).
 * @retval k_ra8_err_invalid_state The block did not read back as erase value.
 * @retval (other)                The first failing re-init / read call's code.
 *
 * @pre ::demo_checkpoint returned k_ra8_ok.
 * @pre The MRAM backing store retains its bytes (non-volatile).
 * @post On success ::s_ftl is freshly initialised and ::s_present is rebound.
 * @post The FTL mapping tables are in their cold-start state.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_reopen_naive(void)
{
  (void)memset(&s_ftl, 0, sizeof(s_ftl));
  (void)memset(s_map, 0, sizeof(s_map));
  (void)memset(s_pblocks, 0, sizeof(s_pblocks));
  (void)memset(&s_present, 0, sizeof(s_present));
  RA8_RETURN_ON_ERROR(ra8_ftl_init(&s_ftl,
                                   &s_bd,
                                   s_map,
                                   (uint32_t)k_demo_ftl_logical,
                                   s_pblocks,
                                   (uint32_t)k_demo_ftl_phys,
                                   s_scratch),
                      s_tag,
                      "ftl reinit");
  RA8_RETURN_ON_ERROR(ra8_ftl_as_blockdev(&s_ftl, &s_present), s_tag, "ftl rebind");
  uint8_t got[(size_t)k_demo_block];
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_read(&s_present, (uint32_t)k_demo_test_lbn, (uint32_t)k_demo_one_block, got),
    s_tag,
    "naive read");
  for (uint32_t i = 0; i < (uint32_t)k_demo_block; ++i) {
    if (got[i] != (uint8_t)k_ra8_io_erase_value_ones) {
      return k_ra8_err_invalid_state;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Act 3b: reload the checkpoint and prove data + mapping survived.
 *
 * @details Reads the checkpoint back from the reserved MRAM block, restores it
 *          with ::ra8_ftl_checkpoint_load, and verifies both that the test
 *          block's last generation reappears byte-for-byte and that its physical
 *          index matches the pre-reset value @p want_phys.
 *
 * @param[in] want_tag  Generation tag of the last pre-reset write.
 * @param[in] want_phys Physical index the test block had before the reset.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Data and mapping were restored intact.
 * @retval k_ra8_err_checksum_mismatch The restored bytes differed.
 * @retval k_ra8_err_invalid_state     The restored physical index differed.
 * @retval (other)                    The first failing read / load call's code.
 *
 * @pre ::demo_reopen_naive returned k_ra8_ok; ::s_ftl is freshly initialised.
 * @pre @p want_phys is the physical index reported before the reset.
 * @post On success the test block reads back its pre-reset contents.
 * @post No MRAM data block is mutated.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_restore(uint32_t want_tag, uint16_t want_phys)
{
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_read(&s_bd, (uint32_t)k_demo_meta_lba, (uint32_t)k_demo_one_block, s_ckbuf),
    s_tag,
    "meta read");
  RA8_RETURN_ON_ERROR(ra8_ftl_checkpoint_load(&s_ftl, s_ckbuf, (uint32_t)k_demo_block),
                      s_tag,
                      "checkpoint load");
  uint8_t want[(size_t)k_demo_block] = {};
  for (uint32_t i = 0; i < (uint32_t)k_demo_block; ++i) {
    want[i] = (uint8_t)((i * (uint32_t)k_demo_seed_mul) +
                        ((uint32_t)k_demo_test_lbn * (uint32_t)k_demo_lbn_mul) + want_tag);
  }
  uint8_t got[(size_t)k_demo_block];
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_read(&s_present, (uint32_t)k_demo_test_lbn, (uint32_t)k_demo_one_block, got),
    s_tag,
    "restored read");
  if (memcmp(got, want, sizeof(want)) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  uint16_t phys = 0U;
  RA8_RETURN_ON_ERROR(ra8_ftl_phys_of(&s_ftl, (uint32_t)k_demo_test_lbn, &phys),
                      s_tag,
                      "restored map");
  if (phys != want_phys) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Print the FTL wear spread (max/min erase counts across physical blocks).
 *
 * @details Best-effort diagnostic: queries ::ra8_ftl_wear_stats and prints the
 *          max and min per-block erase counts so a healthy (tight) spread is
 *          visible. A query failure is reported but does not fail the demo.
 *
 * @return None.
 *
 * @pre ::s_ftl is initialised.
 * @pre ::s_uart is initialised.
 * @post One wear line is queued on the UART stream.
 * @post No FTL state is mutated.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static void demo_report_wear(void)
{
  uint32_t hi = 0U;
  uint32_t lo = 0U;
  if (ra8_ftl_wear_stats(&s_ftl, &hi, &lo) != k_ra8_ok) {
    demo_print("ra8_ftl_demo: wear stats unavailable\r\n");
    return;
  }
  demo_print("ra8_ftl_demo: wear erase-count max ");
  (void)ra8_io_stream_put_u32(&s_uart, hi);
  demo_print(" min ");
  (void)ra8_io_stream_put_u32(&s_uart, lo);
  demo_print("\r\n");
}

/**
 * @brief Run the three acts and report a single PASS/FAIL verdict.
 *
 * @details Sequences wear-levelling, checkpoint, naive re-open, and restore,
 *          short-circuiting on the first failure so the verdict reflects the
 *          first failing act.
 *
 * @return ra8_err_t Error code (k_ra8_ok iff every act passed).
 * @retval k_ra8_ok The full wear-level + power-cycle-survive flow passed.
 * @retval (other) The first failing act's error code.
 *
 * @pre ::demo_open returned k_ra8_ok.
 * @pre The console + MRAM device are ready.
 * @post Diagnostic lines for each act are queued on the UART stream.
 * @post ::s_ftl reflects the restored mapping on success.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_run(void)
{
  uint32_t migrations = 0U;
  uint16_t last_phys  = 0U;
  RA8_RETURN_ON_ERROR(demo_wear_phase(&migrations, &last_phys), s_tag, "wear phase");
  demo_report_wear();
  if (migrations == 0U) {
    return k_ra8_err_invalid_state; /* no relocation observed -- wear-levelling failed */
  }
  RA8_RETURN_ON_ERROR(demo_checkpoint(), s_tag, "checkpoint");
  RA8_RETURN_ON_ERROR(demo_reopen_naive(), s_tag, "reopen naive");
  demo_print("ra8_ftl_demo: naive re-open lost the mapping (reads erase value)\r\n");
  RA8_RETURN_ON_ERROR(
    demo_restore((uint32_t)k_demo_tag_base + (uint32_t)k_demo_writes - (uint32_t)k_demo_one_block,
                 last_phys),
    s_tag,
    "restore");
  return k_ra8_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @details Initialises logging + console, brings up the MRAM volume, runs the
 *          wear-levelling + power-cycle-survival flow, and prints a single
 *          PASS/FAIL verdict.
 *
 * @return Never returns.
 * @retval (none) The function does not return (final `while (true)`).
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre The extra-MRAM region is present (modelled in board_sim, real on silicon).
 * @post Exactly one PASS or FAIL verdict line has been queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 *
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
int main(void)
{
  ra8_log_init();
  demo_setup_or_halt();
  (void)ra8_io_stream_uart_init(&s_uart, &s_ust, (uint8_t)k_demo_uart_chan);
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log into the UART stream too */
  demo_print("ra8_ftl_demo: boot\r\n");

  ra8_err_t e = demo_open();
  if (e == k_ra8_ok) {
    e = demo_run();
  }
  if (e == k_ra8_ok) {
    demo_print("ra8_ftl_demo: wear-level + power-cycle-survive on extra MRAM PASS\r\n");
  } else {
    demo_print("ra8_ftl_demo: FAIL\r\n");
  }
  (void)ra8_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
