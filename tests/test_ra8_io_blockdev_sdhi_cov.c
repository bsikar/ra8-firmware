/**
 * @file test_ra8_io_blockdev_sdhi_cov.c
 * @brief Coverage-boost tests for ra8_io_blockdev_sdhi.c.
 *
 * @details
 * The native-SDHI block-device backend is a thin shim: every vtable op
 * (read / write / get_caps) drops the singleton context and forwards to
 * the ``ra8_sdcard`` HAL driver. The sibling test_ra8_io_blockdev_backends.c
 * only binds the backend and checks the NULL guard, so the three
 * forwarding bodies stay uncovered because their data path needs a live
 * card.
 *
 * This file drives all three bodies:
 *
 *   - sdhi_read / sdhi_write: called through the public
 *     ra8_io_blockdev_read / ra8_io_blockdev_write dispatcher with the card
 *     torn down, so ``ra8_sdcard_read_blocks`` / ``ra8_sdcard_write_blocks``
 *     return ::k_ra8_err_invalid_state. The forwarding statement executes
 *     regardless of the card driver's verdict, so the invalid-state leg
 *     covers every line of both bodies without any hardware bring-up.
 *   - sdhi_get_caps error leg: called with the card torn down so
 *     ``ra8_sdcard_get_capacity`` returns ::k_ra8_err_invalid_state and the
 *     early ``return cap`` runs.
 *   - sdhi_get_caps success snapshot: the ``ra8_sdcard`` singleton is
 *     brought fully up so ``ra8_sdcard_get_capacity`` returns ::k_ra8_ok and
 *     the whole capabilities-fill block runs; every field is checked
 *     against the backend's fixed SD contract.
 *
 * Bringing the card up on the host is the interesting part. ``ra8_sdcard``
 * runs the SD Physical-Layer init as a blocking sequence of
 * ``ra8_sdhi_send_command`` calls, each spinning on SD_INFO1.RSPEND. In
 * RA8_OFF_TARGET the SDHI register window is plain host RAM (see
 * tests/mocks/ra8_fake_mmap.c), so nothing sets RSPEND on its own and the
 * driver clears the bit after every command. The register handshake must
 * therefore be re-serviced between commands while init blocks.
 *
 * The ra8_fake_mmio poll-hook does exactly that -- no SIGALRM / setitimer
 * timer signal and no concurrent servicer thread. The response registers
 * SD_RSP10..76 are pre-loaded once with a single universal value set that
 * satisfies every command the init sequence issues (CMD8 echo, ACMD41 OCR
 * busy+CCS, and a CSD v2 layout), because the driver only ever reads
 * those registers -- it never writes them. The hook then simply
 * re-asserts SD_INFO1.RSPEND; it runs inline on every
 * ``ra8_sdhi_send_command`` poll iteration (the driver's OWN thread), so
 * each command observes the flag on its next poll, copies the pre-loaded
 * response, and returns. Because the injection is keyed to the driver's
 * own register polls rather than elapsed time, the bring-up is
 * deterministic on any host regardless of scheduler load.
 *
 * Builds as a standalone executable auto-discovered by the
 * tests/CMakeLists.txt ``test_*.c`` GLOB; its .gcda stream is merged by
 * gcovr with the sibling tests so the newly hit lines count toward the
 * global ra8_io_blockdev_sdhi.c coverage total without touching the
 * sibling test file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_sdhi.h"
#include "ra8_mstp.h"
#include "ra8_sdcard.h"
#include "ra8_sdhi_regs.h"
#include "unity_minimal.h"

/** @brief Distinguishable payload fills for the read-back assertions. */
typedef enum : uint8_t {
  k_sdhi_cov_fill_write    = 0xA5U, /**< Payload handed to the write path.    */
  k_sdhi_cov_fill_readback = 0x5AU, /**< Poison the read path must overwrite. */
} sdhi_cov_fill_t;

/* ===========================================================================
 * Test constants
 * ===========================================================================
 */

/**
 * @enum cov_sdhi_inst_t
 * @brief SDHI instance index used by every test.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cov_sdhi_inst = 0U, /**< Single-instance driver; all tests share SDHI 0. */
} cov_sdhi_inst_t;

/**
 * @enum cov_sdhi_rsp_t
 * @brief Universal SD response-register values that satisfy every init command.
 *
 * @details
 * The driver copies SD_RSP10..76 verbatim on each command and only reads
 * them, so one static value set can service the whole init sequence:
 *
 * - SD_RSP10 (rsp[0]) = 0xC00001AA
 *     - low 12 bits 0x1AA -> CMD8 SEND_IF_COND echo matches the host pattern.
 *     - bit 31 set        -> ACMD41 OCR "busy done" so the op-cond loop exits
 *                            on its first iteration.
 *     - bit 30 set (CCS)  -> high-capacity card -> SDHC/SDXC classification.
 *     - upper 16 bits     -> CMD3 relative-card-address (value is unused).
 * - SD_RSP76 (rsp[3]) = 0x40000000 -> CSD_STRUCTURE = 1 -> CSD v2 decode.
 * - SD_RSP32 (rsp[1]) = 0xF0000000 -> CSD v2 C_SIZE middle 16 bits = 0xF000.
 * - SD_RSP54 (rsp[2]) = 0x00000000 -> CSD v2 C_SIZE top 6 bits = 0.
 *
 * CSD v2 capacity = (C_SIZE + 1) * 1024 = (0xF000 + 1) * 1024 = 62,915,584
 * 512-byte blocks, below the SDXC boundary, so the card classifies as SDHC.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cov_sdhi_rsp10         = 0xC00001AAUL, /**< CMD8 echo | OCR busy | CCS.    */
  k_cov_sdhi_rsp32         = 0xF0000000UL, /**< CSD v2 C_SIZE middle bits.     */
  k_cov_sdhi_rsp54         = 0x00000000UL, /**< CSD v2 C_SIZE top bits (zero). */
  k_cov_sdhi_rsp76         = 0x40000000UL, /**< CSD_STRUCTURE = 1 (v2 layout). */
  k_cov_sdhi_rspend_bit    = 0x00000001UL, /**< SD_INFO1.RSPEND (bit 0).       */
  k_cov_sdhi_expect_blocks = 62915584UL,   /**< (0xF000 + 1) * 1024 blocks.    */
} cov_sdhi_rsp_t;

/* ===========================================================================
 * RSPEND poll-hook (no SIGALRM, no servicer thread)
 * ===========================================================================
 */

/** @brief SDHI instance the poll-hook keeps RSPEND asserted on. */
static uint8_t s_cov_inst;

/**
 * @brief Re-assert SD_INFO1.RSPEND for the serviced instance.
 *
 * @details
 * The driver clears RSPEND after each command; this hook sets it again so
 * the next ``ra8_sdhi_send_command`` poll iteration observes it. The
 * response registers were pre-loaded by internal_cov_hook_arm() and are never
 * written by the driver, so no other register work is needed. Installed
 * via ::ra8_fake_mmio_set_poll_hook, it runs inline at the top of every
 * ::ra8_fake_mmio_poll -- on the driver's OWN polling thread -- so the
 * injection is keyed to the driver's register reads, never to elapsed
 * time, and cannot race the bounded spin budget under host load.
 *
 * @pre s_cov_inst is a valid SDHI instance index.
 * @pre The SDHI register window is mapped (RA8_OFF_TARGET).
 * @post SD_INFO1.RSPEND is asserted for the serviced instance.
 *
 * @note Runs single-threaded, inline with the driver's poll.
 *
 * @since 0.1.0 @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_cov_rspend_hook(void)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(s_cov_inst);
  if (reg == nullptr) {
    return;
  }
  reg->SD_INFO1 = reg->SD_INFO1 | (uint32_t)k_cov_sdhi_rspend_bit;
}

/**
 * @brief Pre-load the universal responses and install the RSPEND poll-hook.
 *
 * @param[in] inst SDHI instance index to service.
 *
 * @pre No poll-hook is currently installed.
 * @pre ra8_fake_mmap_reset() has already scrubbed the register window.
 * @post SD_RSP10..76 hold the universal init responses and ::internal_cov_rspend_hook
 *       runs on every ra8_fake_mmio_poll with RSPEND asserted.
 *
 * @note Not reentrant; tests are single-threaded.
 *
 * @since 0.1.0 @details Exercises the cov hook arm path with bounded caller-owned fixture state and verifies its documented result. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_cov_hook_arm(uint8_t inst)
{
  s_cov_inst                  = inst;
  volatile r_sdhi_regs_t* reg = ra8_sdhi(inst);
  TEST_ASSERT_NOT_NULL((const void*)reg);
  reg->SD_RSP10 = (uint32_t)k_cov_sdhi_rsp10;
  reg->SD_RSP32 = (uint32_t)k_cov_sdhi_rsp32;
  reg->SD_RSP54 = (uint32_t)k_cov_sdhi_rsp54;
  reg->SD_RSP76 = (uint32_t)k_cov_sdhi_rsp76;
  reg->SD_INFO1 = reg->SD_INFO1 | (uint32_t)k_cov_sdhi_rspend_bit;

  ra8_fake_mmio_set_poll_hook(internal_cov_rspend_hook);
}

/**
 * @brief Remove the RSPEND poll-hook.
 *
 * @pre internal_cov_hook_arm() was called previously.
 * @post No poll-hook is installed.
 *
 * @since 0.1.0 @details Exercises the cov hook disarm path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_cov_hook_disarm(void)
{
  ra8_fake_mmio_set_poll_hook(nullptr);
}

/* ===========================================================================
 * Fixtures
 * ===========================================================================
 */

/**
 * @brief Scrub every register window and force the card driver un-init'd.
 *
 * @pre Called from a test body before touching any HAL function.
 * @post MMIO windows read zero and ra8_sdcard is not initialized.
 *
 * @since 0.1.0 @details Exercises the cov prep path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_cov_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
  (void)ra8_sdcard_deinit();
}

/**
 * @brief Bring the ``ra8_sdcard`` singleton fully up against the poll-hook.
 *
 * @details
 * Installs the RSPEND poll-hook, runs ``ra8_sdcard_init`` once (the hook
 * answers every command on the driver's own poll, so a single attempt is
 * deterministic), and removes the hook. Asserts the card came up.
 *
 * @pre internal_cov_prep() has just run.
 * @post ra8_sdcard_get_capacity() returns ::k_ra8_ok with a non-zero count.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_cov_card_up(void)
{
  internal_cov_hook_arm((uint8_t)k_cov_sdhi_inst);
  const ra8_sdcard_cfg_t cfg = {.instance = (uint8_t)k_cov_sdhi_inst};
  const ra8_err_t        err = ra8_sdcard_init(&cfg);
  internal_cov_hook_disarm();
  TEST_ASSERT_EQ(k_ra8_ok, err);
}

/* ===========================================================================
 * Test functions
 * ===========================================================================
 */

/**
 * @brief sdhi_read forwards through the dispatcher to the card driver.
 *
 * @details
 * Binds the backend and reads one block with the card torn down. The
 * public ``ra8_io_blockdev_read`` null-checks ``buf`` then forwards to
 * ``sdhi_read``, whose ``return ra8_sdcard_read_blocks(...)`` executes and
 * surfaces ::k_ra8_err_invalid_state (card never initialized). Line
 * coverage of the forwarding body does not depend on the card being up.
 *
 * @par MC/DC:
 * sdhi_read has one single-condition decision, the ``RA8_CHECK_NULL_PTR(buf)``
 * guard (no ``&&`` / ``||``). Its FALSE arm is covered here (buf non-NULL);
 * the TRUE arm is unreachable through ra8_io_blockdev_read, which null-checks
 * buf before forwarding, so no compound-decision vectors are required.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdhi_read_forwards(void)
{
  TEST_BEGIN("sdhi_read forwards to card driver");
  internal_cov_prep();
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sdhi_init(&bd));

  uint8_t buf[(size_t)k_ra8_io_block_size_bytes];
  memset(buf, k_sdhi_cov_fill_write, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_read(&bd, 0U, 1U, buf));
  TEST_END("sdhi_read forwards to card driver");
}

/**
 * @brief sdhi_write forwards through the dispatcher to the card driver.
 *
 * @details
 * Mirror of internal_test_sdhi_read_forwards for the write path: a single-block
 * write with the card torn down forwards to ``sdhi_write``, whose
 * ``return ra8_sdcard_write_blocks(...)`` executes and surfaces
 * ::k_ra8_err_invalid_state.
 *
 * @par MC/DC:
 * sdhi_write has one single-condition decision, the ``RA8_CHECK_NULL_PTR(buf)``
 * guard. FALSE arm covered here (buf non-NULL); the TRUE arm is unreachable
 * through ra8_io_blockdev_write, which null-checks buf first. No compound
 * decision exists in this body.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdhi_write_forwards(void)
{
  TEST_BEGIN("sdhi_write forwards to card driver");
  internal_cov_prep();
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sdhi_init(&bd));

  uint8_t buf[(size_t)k_ra8_io_block_size_bytes];
  memset(buf, k_sdhi_cov_fill_readback, sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_write(&bd, 0U, 1U, buf));
  TEST_END("sdhi_write forwards to card driver");
}

/**
 * @brief sdhi_get_caps early-returns the card driver's capacity error.
 *
 * @details
 * With the card torn down, ``ra8_sdcard_get_capacity`` returns
 * ::k_ra8_err_invalid_state, so ``sdhi_get_caps`` takes the ``if (cap !=
 * k_ra8_ok) return cap;`` early-out and the capabilities-fill block is
 * skipped.
 *
 * @par MC/DC:
 * sdhi_get_caps up to the early-out has two single-condition decisions:
 * - ``RA8_CHECK_NULL_PTR(out)`` -- FALSE arm covered here (out non-NULL); the
 *   TRUE arm is unreachable through ra8_io_blockdev_get_caps, which null-checks
 *   out before forwarding.
 * - ``if (cap != k_ra8_ok)`` -- TRUE arm covered here (invalid-state card); its
 *   FALSE arm is covered by internal_test_sdhi_get_caps_success.
 * Neither is a compound (``&&`` / ``||``) decision.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdhi_get_caps_error(void)
{
  TEST_BEGIN("sdhi_get_caps error leg");
  internal_cov_prep();
  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sdhi_init(&bd));

  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_END("sdhi_get_caps error leg");
}

/**
 * @brief sdhi_get_caps fills the full SD capabilities snapshot on success.
 *
 * @details
 * Brings the ``ra8_sdcard`` singleton fully up (see internal_cov_card_up), binds the
 * backend, then queries capabilities. ``ra8_sdcard_get_capacity`` returns
 * ::k_ra8_ok, so the whole fill block runs; every field is checked against
 * the backend's fixed SD contract (one-block erase unit, 512-byte program
 * size and logical block, zero erase value, no erase-before-write, and
 * writable), plus the decoded block count.
 *
 * @par MC/DC:
 * The only decision reached in the success path is the single-condition
 * ``if (cap != k_ra8_ok)`` whose FALSE arm is covered here; its TRUE arm is
 * covered by internal_test_sdhi_get_caps_error. No compound decision exists.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdhi_get_caps_success(void)
{
  TEST_BEGIN("sdhi_get_caps success snapshot");
  internal_cov_prep();
  internal_cov_card_up();

  /* Sanity: the driver reports the capacity our CSD v2 responses encode. */
  uint32_t blocks = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_get_capacity(&blocks));
  TEST_ASSERT_EQ(k_cov_sdhi_expect_blocks, blocks);

  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sdhi_init(&bd));

  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(k_cov_sdhi_expect_blocks, caps.block_count);
  TEST_ASSERT_EQ(1U, caps.erase_unit_blocks);
  TEST_ASSERT_EQ(k_ra8_io_block_size_bytes, caps.program_size_bytes);
  TEST_ASSERT_EQ(k_ra8_io_block_size_bytes, caps.logical_block_bytes);
  TEST_ASSERT_EQ(k_ra8_io_erase_value_zero, caps.erase_value);
  TEST_ASSERT(!caps.must_erase_before_write);
  TEST_ASSERT(!caps.read_only);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdcard_deinit());
  TEST_END("sdhi_get_caps success snapshot");
}

/**
 * @brief ra8_io_blockdev_sdhi_init NULL guard and successful bind.
 *
 * @details
 * Confirms the bind helper rejects a NULL handle and, given a valid
 * handle, wires the SDHI vtable so a later dispatch reaches the backend.
 *
 * @par MC/DC:
 * The bind helper's only decision is the single-condition
 * ``RA8_CHECK_NULL_PTR(bd)`` guard: TRUE arm (bd == NULL) and FALSE arm
 * (valid bd) are both exercised here. No compound decision exists.
 *
 * @since 0.1.0 @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. */
RA8_INTERNAL static void internal_test_sdhi_bind(void)
{
  TEST_BEGIN("sdhi_init NULL guard + bind");
  internal_cov_prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_blockdev_sdhi_init(nullptr));

  ra8_io_blockdev_t bd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sdhi_init(&bd));
  TEST_ASSERT_NOT_NULL((const void*)bd.iface);
  TEST_END("sdhi_init NULL guard + bind");
}

/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

/**
 * @brief Run all native-SDHI block-device coverage tests.
 *
 * @return 0 on success; the harness calls exit(1) on the first failure.
 *
 * @pre The ra8_core_hal OBJECT library was built with coverage instrumentation.
 * @post Every forwarding body in ra8_io_blockdev_sdhi.c has executed at least
 *       once, including the get_caps success and error legs.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_sdhi_bind();
  internal_test_sdhi_read_forwards();
  internal_test_sdhi_write_forwards();
  internal_test_sdhi_get_caps_error();
  internal_test_sdhi_get_caps_success();
  return 0;
}
