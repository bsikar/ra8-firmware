/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file xspi_test_util.h
 * @brief Shared fixture for the test_ra8_xspi* suite: instance/address
 *        test constants and the prep_flash() reset that reinstalls the
 *        register-level NOR flash model
 *
 * @details Header-only (all definitions `static`) so each split
 * test_ra8_xspi* binary carries its own private copy; the
 * tests/CMakeLists.txt auto-glob stays free of non-test .c files.
 * Split out of test_ra8_xspi.c when the suite was divided into
 * core / program / ctrl binaries.
 */

#pragma once

#include <stdint.h>

#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_fake_xspi_flash.h"

typedef enum : uint8_t {
  k_test_xspi_bad_instance = 99U, /**< Test XSPI bad instance. */
  k_test_xspi_valid_inst0  = 0U,  /**< Test XSPI valid inst0.  */
  k_test_xspi_valid_inst1  = 1U,  /**< Test XSPI valid inst1.  */
  k_test_xspi_too_many     = 20U, /**< Test XSPI too many.     */
} test_xspi_inst_t;

typedef enum : uint32_t {
  k_test_xspi_flash_addr_start    = 0U,           /**< Test XSPI flash address start.          */
  k_test_xspi_flash_addr_middle   = 128U,         /**< Test XSPI flash address middle.         */
  k_test_xspi_flash_addr_pagetail = 250U,         /**< 6 bytes below the 256-byte page end.    */
  k_test_xspi_flash_addr_overflow = 0x10000000UL, /**< Past 2^24 (3-byte space).               */
  k_test_xspi_flash_addr_near_top = 0xFFFFF8UL,   /**< 8 bytes below 2^24.                     */
  k_test_xspi_len_zero            = 0U,           /**< Forces C1=T (short-circuits).           */
  k_test_xspi_len_small           = 16U,          /**< Test XSPI length small.                 */
  k_test_xspi_len_multipage       = 320U,         /**< > 256-byte page; crosses from addr 128. */
  k_test_xspi_len_too_big         = 8192U,        /**< Test XSPI length too big.               */
  k_test_xspi_expected_jedec      = 0x9D5A1AUL,   /**< ISSI IS25LX512M (on-board).             */
  k_test_xspi_expected_status     = 0x02U,        /**< WEL=1 (model's implicit latch).         */
  k_test_xspi_wip_busy_polls      = 3U,           /**< RDSR busy polls for the retry.          */
  k_test_xspi_nth_wren            = 0U,           /**< 1st INTS wait: the WREN command.        */
  k_test_xspi_nth_pp_or_se        = 1U,           /**< 2nd INTS wait: the PP / SE.             */
  k_test_xspi_nth_rdsr            = 2U,           /**< 3rd INTS wait: the WIP RDSR.            */
} test_xspi_vals_t;

/**
 * @brief Reset the fake world and install the register-level NOR model.
 *
 * @details
 * The ``ra8_fake_mmio_reset`` clears any installed poll hook, so the
 * ``ra8_fake_xspi_flash`` model (which services every ``TRREQ`` kick and
 * backs the flash data) must be re-installed afterwards, per test case.
 *
 * @pre Host test binary (``RA8_OFF_TARGET`` + ``UNIT_TEST``).
 * @pre No other thread touches the xSPI registers (single-threaded test).
 * @post Register RAM is zeroed and the seam holds no armed faults.
 * @post The NOR model is installed with fully-erased backing arrays.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
static inline void prep_flash(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_fake_xspi_flash_install();
}
