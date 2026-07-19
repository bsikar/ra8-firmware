/**
 * @file test_ra8_usb_hmsc.c
 * @brief Unit tests for the native USB host-side MSC class layer
 *
 * @details
 * Exercises the public contract of `ra8_usb_hmsc` against the register
 * simulator: lifecycle (init/close), the polled `ra8_usb_hmsc_enumerate`
 * failure path (the dumb register mirror cannot answer GET_DESCRIPTOR,
 * so a full attach is end-to-end hardware territory -- see
 * `examples/ek_ra8d2/hw_pending/usb_host_msc_browse` and
 * `examples/ek_ra8d2/hw_validated/manual/usb_host_file_ops` for the
 * validated ladders at FS and HS), the pre-init/pre-attach guards on
 * every entry point, and the pure protocol units (CBW build / CSW
 * decode) byte-for-byte against USB MSC BBB rev 1.0.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_hmsc_csw_t
 * @brief Command Status Wrapper bytes the host-side arms build and corrupt.
 *
 * @details
 * A CSW opens with the ASCII signature "USBS" and echoes the CBW tag. The
 * `k_t_tag_*` bytes spell 0xCAFEBABE little-endian: a distinctive tag that is
 * obviously wrong if the driver echoes the wrong wrapper.
 */
typedef enum : uint8_t {
  k_t_csw_sig_b0     = 0x55U, /**< CSW signature byte 0, ASCII 'U'.          */
  k_t_csw_sig_b1     = 0x53U, /**< Signature bytes 1 and 3, ASCII 'S'.       */
  k_t_csw_sig_b2     = 0x42U, /**< Signature byte 2, ASCII 'B'.              */
  k_t_tag_b0         = 0xBEU, /**< Echoed tag byte 0.                        */
  k_t_tag_b1         = 0xBAU, /**< Echoed tag byte 1.                        */
  k_t_tag_b2         = 0xFEU, /**< Echoed tag byte 2.                        */
  k_t_tag_b3         = 0xCAU, /**< Echoed tag byte 3.                        */
  k_t_status_bad     = 0x99U, /**< A status outside the 0..2 range the spec
                                   defines, which the driver must reject.     */
  k_t_scsi_read10    = 0x28U, /**< SCSI READ(10) opcode.                     */
  k_t_cdb_len_10     = 10U,   /**< CDB length of the 10-byte SCSI commands.  */
  k_t_le32_hi_shift  = 24U,   /**< Shift for the top byte of the 32-bit tag. */
  k_t_byte_mask      = 0xFFU, /**< Low-byte mask while serialising it.       */
  k_t_csw_off_tag_b1 = 5U,    /**< Tag byte 1 offset in a bare CSW buffer.   */
  k_t_csw_off_tag_b3 = 7U,    /**< Tag byte 3 offset in a bare CSW buffer.   */
} t_hmsc_csw_t;

typedef enum : uint8_t {
  k_test_cbw_off_signature   = 0U,  /**< Test cbw off signature.   */
  k_test_cbw_off_tag         = 4U,  /**< Test cbw off tag.         */
  k_test_cbw_off_data_length = 8U,  /**< Test cbw off data length. */
  k_test_cbw_off_flags       = 12U, /**< Test cbw off flags.       */
  k_test_cbw_off_lun         = 13U, /**< Test cbw off lun.         */
  k_test_cbw_off_cdb_length  = 14U, /**< Test cbw off cdb length.  */
  k_test_cbw_off_cdb         = 15U, /**< Test cbw off cdb.         */
  k_test_cbw_len             = 31U, /**< Test cbw length.          */
  k_test_csw_off_signature   = 0U,  /**< Test csw off signature.   */
  k_test_csw_off_tag         = 4U,  /**< Test csw off tag.         */
  k_test_csw_off_status      = 12U, /**< Test csw off status.      */
  k_test_csw_len             = 13U, /**< Test csw length.          */
} test_hmsc_layout_t;

/** @brief SYSSTS0.LNST J-state mirror value (D+ pulled up, FS idle). */
static const uint16_t k_test_lnst_j_state = 0x0001U;

static uint32_t              s_attach_count;
static ra8_usb_hmsc_device_t s_attach_last_device;
static void*                 s_attach_last_ctx;
static const uintptr_t       k_test_hmsc_ctx_token = 0xDEADBEEFU;

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_hmsc_close();
  s_attach_count       = 0U;
  s_attach_last_device = (ra8_usb_hmsc_device_t){};
  s_attach_last_ctx    = nullptr;
}

static void stub_on_attach(void* ctx, const ra8_usb_hmsc_device_t* device)
{
  ++s_attach_count;
  s_attach_last_ctx    = ctx;
  s_attach_last_device = *device;
}

/* ---- Lifecycle ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_fs_returns_ok(void)
{
  TEST_BEGIN("ra8_usb_hmsc_init FS returns k_ra8_ok");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));

  /* Host-mode SYSCFG should have DCFM and DRPD set, not DPRPU. */
  volatile r_usb_regs_t* reg   = ra8_usb_fs();
  const uint16_t         dcfm  = (uint16_t)(1U << k_ra8_syscfg_bit_dcfm);
  const uint16_t         drpd  = (uint16_t)(1U << k_ra8_syscfg_bit_drpd);
  const uint16_t         dprpu = (uint16_t)(1U << k_ra8_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dcfm) != 0U);
  TEST_ASSERT((reg->SYSCFG & drpd) != 0U);
  TEST_ASSERT_EQ(0, (reg->SYSCFG & dprpu));

  TEST_END("ra8_usb_hmsc_init FS returns k_ra8_ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_hmsc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_hmsc_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_init(void)
{
  TEST_BEGIN("ra8_usb_hmsc_close before init returns invalid_state");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_close());
  TEST_END("ra8_usb_hmsc_close before init returns invalid_state");
}

/* ---- Enumerate failure path (no answering device in the simulator) ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 *
 * @note The register mirror never answers GET_DESCRIPTOR, so the hunt
 * exhausts its (reset, address) attempts and `ra8_usb_hmsc_enumerate`
 * must fail without firing the attach callback or flipping the
 * attached state. The LNST mirror is pre-set to a J-state so the
 * attach wait exits immediately (the wait is also iteration-bounded
 * for frozen-tick builds like this one). A full successful ladder is
 * hardware-validated (FS + HS golden runs in the example READMEs).
 */
static void test_enumerate_no_answering_device_fails(void)
{
  TEST_BEGIN("enumerate fails cleanly when nothing answers, callback never fires");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_hmsc_attach_callback(stub_on_attach, (void*)k_test_hmsc_ctx_token));

  /* Pretend a device pulled D+ up so the attach wait exits at once. */
  ra8_usb_fs()->SYSSTS0 = k_test_lnst_j_state;

  ra8_usb_hmsc_device_t device = {};
  TEST_ASSERT(ra8_usb_hmsc_enumerate(&device) != k_ra8_ok);
  TEST_ASSERT_EQ(0U, s_attach_count);

  /* SCSI entry points must still reject: not attached. */
  ra8_usb_hmsc_inquiry_response_t resp = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_inquiry(0U, &resp));

  TEST_END("enumerate fails cleanly when nothing answers, callback never fires");
}

/* ---- Pre-init / pre-attach guards on every entry point ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_init_guards(void)
{
  TEST_BEGIN("attach_callback / enumerate / SCSI ops reject pre-init");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_attach_callback(stub_on_attach, nullptr));
  ra8_usb_hmsc_device_t device = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_enumerate(&device));

  ra8_usb_hmsc_inquiry_response_t resp        = {};
  uint32_t                        block_count = 0U;
  uint32_t                        block_size  = 0U;
  uint8_t                         buf[16]     = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_inquiry(0U, &resp));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hmsc_read_capacity(0U, &block_count, &block_size));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_read10(0U, 0U, 1U, buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_write10(0U, 0U, 1U, buf));

  TEST_END("attach_callback / enumerate / SCSI ops reject pre-init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pre_attach_guards(void)
{
  TEST_BEGIN("SCSI ops reject pre-attach (post-init)");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));

  ra8_usb_hmsc_inquiry_response_t resp        = {};
  uint32_t                        block_count = 0U;
  uint32_t                        block_size  = 0U;
  uint8_t                         buf[16]     = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_inquiry(0U, &resp));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_hmsc_read_capacity(0U, &block_count, &block_size));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_read10(0U, 0U, 1U, buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_write10(0U, 0U, 1U, buf));

  TEST_END("SCSI ops reject pre-attach (post-init)");
}

/* ---- SCSI op null-arg + envelope rejection (argument checks precede
 *      the attached-state gate, so no attach is needed) ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_scsi_null_arg_rejection(void)
{
  TEST_BEGIN("inquiry / read_capacity / read10 / write10 reject NULL / zero count");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));

  uint32_t block_count = 0U;
  uint32_t block_size  = 0U;
  uint8_t  buf[16]     = {};

  /* NULL out / in pointers (checked before the attached gate). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_inquiry(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_read_capacity(0U, nullptr, &block_size));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_read_capacity(0U, &block_count, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_read10(0U, 0U, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_write10(0U, 0U, 1U, nullptr));

  /* Zero block_count rejected by read10 / write10 (also pre-gate). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_read10(0U, 0U, 0U, buf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_write10(0U, 0U, 0U, buf));

  TEST_END("inquiry / read_capacity / read10 / write10 reject NULL / zero count");
}

/* ---- CBW signature is 0x43425355 in the constructed CBW header ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_build_cbw_signature_layout(void)
{
  TEST_BEGIN("ra8_usb_hmsc_build_cbw lays out CBW per BBB rev 1.0 sec 5.1");

  uint8_t cdb[k_t_cdb_len_10] = {};
  cdb[0]                         = k_t_scsi_read10; /* SCSI READ(10) opcode. */
  uint8_t cbw[k_test_cbw_len]    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_build_cbw(0U, 512U, true, cdb, 10U, cbw));

  /* dCBWSignature = 'USBC' little-endian = 0x55, 0x53, 0x42, 0x43. */
  TEST_ASSERT_EQ(0x55U, cbw[k_test_cbw_off_signature + 0U]);
  TEST_ASSERT_EQ(0x53U, cbw[k_test_cbw_off_signature + 1U]);
  TEST_ASSERT_EQ(0x42U, cbw[k_test_cbw_off_signature + 2U]);
  TEST_ASSERT_EQ(0x43U, cbw[k_test_cbw_off_signature + 3U]);

  /* dCBWDataTransferLength = 512 little-endian = 0x00, 0x02, 0x00, 0x00. */
  TEST_ASSERT_EQ(0x00U, cbw[k_test_cbw_off_data_length + 0U]);
  TEST_ASSERT_EQ(0x02U, cbw[k_test_cbw_off_data_length + 1U]);
  TEST_ASSERT_EQ(0x00U, cbw[k_test_cbw_off_data_length + 2U]);
  TEST_ASSERT_EQ(0x00U, cbw[k_test_cbw_off_data_length + 3U]);

  /* bmCBWFlags = 0x80 (data IN). */
  TEST_ASSERT_EQ(0x80U, cbw[k_test_cbw_off_flags]);
  /* bCBWLUN = 0. */
  TEST_ASSERT_EQ(0x00U, cbw[k_test_cbw_off_lun]);
  /* bCBWCBLength = 10. */
  TEST_ASSERT_EQ(0x0AU, cbw[k_test_cbw_off_cdb_length]);
  /* CBWCB[0] echoed. */
  TEST_ASSERT_EQ(0x28U, cbw[k_test_cbw_off_cdb + 0U]);

  /* Out direction flips bit 7. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_build_cbw(1U, 256U, false, cdb, 10U, cbw));
  TEST_ASSERT_EQ(0x00U, cbw[k_test_cbw_off_flags]);
  TEST_ASSERT_EQ(0x01U, cbw[k_test_cbw_off_lun]);

  TEST_END("ra8_usb_hmsc_build_cbw lays out CBW per BBB rev 1.0 sec 5.1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_build_cbw_arg_rejection(void)
{
  TEST_BEGIN("ra8_usb_hmsc_build_cbw rejects NULL / out-of-range");
  uint8_t cdb[k_t_cdb_len_10] = {};
  uint8_t cbw[k_test_cbw_len]    = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_build_cbw(0U, 0U, true, nullptr, 10U, cbw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_build_cbw(0U, 0U, true, cdb, 10U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_build_cbw(99U, 0U, true, cdb, 10U, cbw));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_build_cbw(0U, 0U, true, cdb, 0U, cbw));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_build_cbw(0U, 0U, true, cdb, 99U, cbw));
  TEST_END("ra8_usb_hmsc_build_cbw rejects NULL / out-of-range");
}

/* ---- CSW status decoding (0x00 / 0x01 / 0x02) ---- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_decode_csw_status(void)
{
  TEST_BEGIN("ra8_usb_hmsc_decode_csw decodes 0x00 / 0x01 / 0x02 status bytes");

  /* Build a valid CSW: 'USBS' little-endian, tag = 0xCAFEBABE,
   * residue = 0, status = the case under test. */
  uint8_t                   csw[k_test_csw_len] = {};
  ra8_usb_hmsc_csw_status_t status_out          = k_ra8_hmsc_csw_status_passed;

  /* dCSWSignature = 'USBS' little-endian = 0x55, 0x53, 0x42, 0x53. */
  csw[k_test_csw_off_signature + 0U] = k_t_csw_sig_b0;
  csw[k_test_csw_off_signature + 1U] = k_t_csw_sig_b1;
  csw[k_test_csw_off_signature + 2U] = k_t_csw_sig_b2;
  csw[k_test_csw_off_signature + 3U] = k_t_csw_sig_b1;
  /* dCSWTag = 0xCAFEBABE little-endian. */
  csw[k_test_csw_off_tag + 0U] = k_t_tag_b0;
  csw[k_test_csw_off_tag + 1U] = k_t_tag_b1;
  csw[k_test_csw_off_tag + 2U] = k_t_tag_b2;
  csw[k_test_csw_off_tag + 3U] = k_t_tag_b3;

  /* status = 0x00 (passed). */
  csw[k_test_csw_off_status] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &status_out));
  TEST_ASSERT_EQ(k_ra8_hmsc_csw_status_passed, status_out);

  /* status = 0x01 (failed). */
  csw[k_test_csw_off_status] = 0x01U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &status_out));
  TEST_ASSERT_EQ(k_ra8_hmsc_csw_status_failed, status_out);

  /* status = 0x02 (phase error). */
  csw[k_test_csw_off_status] = 0x02U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &status_out));
  TEST_ASSERT_EQ(k_ra8_hmsc_csw_status_phase_error, status_out);

  /* status = 0x99 -> rejected as bogus. */
  csw[k_test_csw_off_status] = k_t_status_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &status_out));

  /* Tag mismatch -> invalid_arg. */
  csw[k_test_csw_off_status] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_decode_csw(csw, 0xDEADBEEFU, &status_out));

  /* Bad signature -> invalid_arg. */
  csw[k_test_csw_off_signature + 0U] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &status_out));

  /* NULL pointers. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_decode_csw(nullptr, 0U, &status_out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_hmsc_decode_csw(csw, 0U, nullptr));

  TEST_END("ra8_usb_hmsc_decode_csw decodes status bytes");
}

/**
 * @test test_mcdc_hmsc
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_hmsc.c.
 *
 * Decision A (hmsc_init speed gate, 2 conds):
 *   `(speed != FS) && (speed != HS)` -- N+1=3.
 * Decision B (build_cbw cdb_len envelope, 2 conds):
 *   `(cdb_len == 0) || (cdb_len > 16)` -- N+1=3:
 *   - V1 cdb_len=0  -> C1=T (short circuit)         -> dec=T (invalid_arg)
 *   - V2 cdb_len=6  -> C1=F, C2=F                   -> dec=F (ok)
 *   - V3 cdb_len=20 -> C1=F, C2=T                   -> dec=T (invalid_arg)
 * Decision C (3-condition OR chain in `ra8_usb_hmsc_decode_csw` status
 *   validation): per DO-178C 6.4.4.3 representative-subset for a
 *   side-effect-free OR -- 3 lone-true vectors (passed / failed /
 *   phase_error) + 1 all-false (0xFF).
 */
static void test_mcdc_hmsc(void)
{
  TEST_BEGIN("hmsc MC/DC: init / build_cbw / decode_csw status OR chain");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_init((ra8_usb_speed_t)9U));

  /* Decision B: build_cbw cdb_len envelope. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init(k_ra8_usb_speed_fs));
  uint8_t       cbw[k_test_cbw_len] = {};
  const uint8_t cdb6[6]             = {0x12U, 0, 0, 0, 16U, 0};
  /* B-V2: cdb_len=6 -> ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_build_cbw(0U, 36U, true, cdb6, 6U, cbw));
  /* B-V1: cdb_len=0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_build_cbw(0U, 0U, false, cdb6, 0U, cbw));
  /* B-V3: cdb_len=20 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_build_cbw(0U, 0U, false, cdb6, 20U, cbw));

  /* Decision C: decode_csw status OR chain. */
  uint8_t        csw[k_test_csw_len] = {};
  const uint32_t tag                 = 0x12345678U;
  /* Build a valid CSW header. */
  csw[k_test_csw_off_signature + 0U]   = k_t_csw_sig_b0;
  csw[k_test_csw_off_signature + 1U]   = k_t_csw_sig_b1;
  csw[k_test_csw_off_signature + 2U]   = k_t_csw_sig_b2;
  csw[k_test_csw_off_signature + 3U]   = k_t_csw_sig_b1;
  csw[k_test_csw_off_tag + 0U]         = (uint8_t)(tag & k_t_byte_mask);
  csw[k_test_csw_off_tag + 1U]         = (uint8_t)((tag >> 8U) & k_t_byte_mask);
  csw[k_test_csw_off_tag + 2U]         = (uint8_t)((tag >> 16U) & k_t_byte_mask);
  csw[k_test_csw_off_tag + 3U]         = (uint8_t)((tag >> k_t_le32_hi_shift) & k_t_byte_mask);
  ra8_usb_hmsc_csw_status_t out_status = (ra8_usb_hmsc_csw_status_t)k_t_byte_mask;

  csw[k_test_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_passed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, tag, &out_status));
  csw[k_test_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_failed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, tag, &out_status));
  csw[k_test_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_phase_error;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, tag, &out_status));
  /* All-false vector: bogus status byte. */
  csw[k_test_csw_off_status] = k_t_byte_mask;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_decode_csw(csw, tag, &out_status));

  TEST_END("hmsc MC/DC: init / build_cbw / decode_csw status OR chain");
}

/**
 * @test test_mcdc_hmsc_decode_csw_status_and_chain
 *
 * @par MC/DC:
 * Decision (ra8_usb_hmsc_decode_csw status validation):
 *   ``(status_byte != PASSED) && (status_byte != FAILED) &&
 *    (status_byte != PHASE_ERROR)``
 * (3 conditions, AND-chain). Exercised end-to-end via the public
 * ra8_usb_hmsc_decode_csw API with byte-tampered CSW blobs.
 *
 * @par DO-178C 6.4.4.3 representative-subset rationale:
 * Full short-circuit MC/DC for an N=3 AND-chain requires N+1 = 4
 * vectors. Canonical short-circuit set:
 * - V1 status=0x00 (PASSED) -> C1=F shorts.            Decision F -> ok.
 * - V2 status=0x01 (FAILED) -> C1=T,C2=F shorts.       Decision F -> ok.
 * - V3 status=0x02 (PHASE)  -> C1=T,C2=T,C3=F.         Decision F -> ok.
 * - V4 status=0x99 unknown  -> all T.                  Decision T -> invalid_arg.
 *
 * Pairs isolating each condition:
 *   C1: V1 vs V4. C2: V2 vs V4. C3: V3 vs V4.
 */
static void test_mcdc_hmsc_decode_csw_status_and_chain(void)
{
  TEST_BEGIN("hmsc MC/DC: decode_csw 3-cond status AND-chain");
  uint8_t csw[(size_t)k_test_csw_len] = {};
  csw[0]                              = k_t_csw_sig_b0;
  csw[1]                              = k_t_csw_sig_b1;
  csw[2]                              = k_t_csw_sig_b2;
  csw[3]                              = k_t_csw_sig_b1;
  csw[4]                              = k_t_tag_b0;
  csw[k_t_csw_off_tag_b1]               = k_t_tag_b1;
  csw[6]                              = k_t_tag_b2;
  csw[k_t_csw_off_tag_b3]               = k_t_tag_b3;

  ra8_usb_hmsc_csw_status_t out = k_ra8_hmsc_csw_status_passed;

  csw[(size_t)k_test_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_passed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &out));
  TEST_ASSERT_EQ(k_ra8_hmsc_csw_status_passed, out);

  csw[(size_t)k_test_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_failed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &out));
  TEST_ASSERT_EQ(k_ra8_hmsc_csw_status_failed, out);

  csw[(size_t)k_test_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_phase_error;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &out));
  TEST_ASSERT_EQ(k_ra8_hmsc_csw_status_phase_error, out);

  csw[(size_t)k_test_csw_off_status] = k_t_status_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_decode_csw(csw, 0xCAFEBABEU, &out));

  TEST_END("hmsc MC/DC: decode_csw 3-cond status AND-chain");
}

int32_t main(void)
{
  test_init_fs_returns_ok();
  test_init_bad_speed();
  test_close_without_init();
  test_enumerate_no_answering_device_fails();
  test_pre_init_guards();
  test_pre_attach_guards();
  test_scsi_null_arg_rejection();
  test_build_cbw_signature_layout();
  test_build_cbw_arg_rejection();
  test_decode_csw_status();
  test_mcdc_hmsc();
  test_mcdc_hmsc_decode_csw_status_and_chain();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_hmsc.c\n");
  return 0;
}
