/**
 * @file test_ra8_usb_hmsc_bot_cov.c
 * @brief Deterministic white-box coverage for the host-MSC BOT command tail.
 * @details Compiles a private copy of the production BOT state machine and
 * redirects only its bulk transport calls to a bounded synchronous script.
 * This models SIE completions on the driver thread without timers, worker
 * threads, or changes to production behavior.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_t_script_cap        = 16U, /**< Maximum scripted calls per direction. */
  k_t_cdb_len           = 6U,  /**< Small valid SCSI CDB length.          */
  k_t_data_len          = 8U,  /**< READ CAPACITY payload length.         */
  k_t_csw_len           = 13U, /**< BOT command-status wrapper length.    */
  k_t_small_payload_len = 4U   /**< Short data-stage payload length.      */
} t_bot_size_t;

typedef enum : uint32_t {
  k_t_initial_tag = 0x10203040U, /**< Distinct BOT tag echoed by the CSW. */
  k_t_last_lba    = 3U,          /**< Capacity response: final LBA.       */
  k_t_block_count = 4U,          /**< Expected final-LBA + one.           */
  k_t_block_size  = 512U         /**< Capacity response: 512-byte blocks. */
} t_bot_value_t;

/** @brief Scripted bulk-OUT outcomes in call order. */
static ra8_err_t s_out_results[k_t_script_cap];
/** @brief Number of valid entries in ::s_out_results. */
static uint8_t s_out_count;
/** @brief Next bulk-OUT script entry. */
static uint8_t s_out_index;
/** @brief Scripted bulk-IN outcomes in call order. */
static ra8_err_t s_in_results[k_t_script_cap];
/** @brief Scripted bulk-IN payload pointers. */
static const uint8_t* s_in_payloads[k_t_script_cap];
/** @brief Scripted bulk-IN payload lengths. */
static uint16_t s_in_lengths[k_t_script_cap];
/** @brief Number of valid bulk-IN entries. */
static uint8_t s_in_count;
/** @brief Next bulk-IN script entry. */
static uint8_t s_in_index;

RA8_INTERNAL static ra8_err_t
internal_mock_bulk_out(ra8_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len);
RA8_INTERNAL static ra8_err_t internal_mock_bulk_in(ra8_usb_speed_t speed,
                                                    uint8_t         pipe_num,
                                                    uint8_t*        buf,
                                                    uint16_t        max_len,
                                                    uint16_t*       out_received);

// NOLINTBEGIN(readability-identifier-naming) -- exact external symbols form the include-time seam.
/** @brief Redirect bulk-OUT transport to the deterministic test script. */
#define ra8_usb_host_bulk_out internal_mock_bulk_out
/** @brief Redirect bulk-IN transport to the deterministic test script. */
#define ra8_usb_host_bulk_in internal_mock_bulk_in
/** @brief Rename the copied host-MSC state object for private linkage. */
#define g_usb_hmsc_state g_usb_hmsc_state_bot_cov
/** @brief Rename the copied CBW builder entry point. */
#define ra8_usb_hmsc_build_cbw ra8_usb_hmsc_build_cbw_bot_cov
/** @brief Rename the copied CSW decoder entry point. */
#define ra8_usb_hmsc_decode_csw ra8_usb_hmsc_decode_csw_bot_cov
/** @brief Rename the copied host-MSC initializer for private linkage. */
#define ra8_usb_hmsc_init ra8_usb_hmsc_init_bot_cov
/** @brief Rename the copied host-MSC close entry point. */
#define ra8_usb_hmsc_close ra8_usb_hmsc_close_bot_cov
/** @brief Rename the copied host-MSC callback registration entry point. */
#define ra8_usb_hmsc_attach_callback ra8_usb_hmsc_attach_callback_bot_cov
/** @brief Rename the copied host-MSC INQUIRY entry point. */
#define ra8_usb_hmsc_inquiry ra8_usb_hmsc_inquiry_bot_cov
/** @brief Rename the copied host-MSC READ CAPACITY entry point. */
#define ra8_usb_hmsc_read_capacity ra8_usb_hmsc_read_capacity_bot_cov
/** @brief Rename the copied host-MSC READ(10) entry point. */
#define ra8_usb_hmsc_read10 ra8_usb_hmsc_read10_bot_cov
/** @brief Rename the copied host-MSC WRITE(10) entry point. */
#define ra8_usb_hmsc_write10 ra8_usb_hmsc_write10_bot_cov
// NOLINTEND(readability-identifier-naming)

#include "ra8_usb_hmsc.c" // NOLINT(bugprone-suspicious-include) -- white-box transport seam

#undef ra8_usb_host_bulk_out
#undef ra8_usb_host_bulk_in
#undef g_usb_hmsc_state
#undef ra8_usb_hmsc_build_cbw
#undef ra8_usb_hmsc_decode_csw
#undef ra8_usb_hmsc_init
#undef ra8_usb_hmsc_close
#undef ra8_usb_hmsc_attach_callback
#undef ra8_usb_hmsc_inquiry
#undef ra8_usb_hmsc_read_capacity
#undef ra8_usb_hmsc_read10
#undef ra8_usb_hmsc_write10

/**
 * @brief Return the next scripted bulk-OUT result.
 * @details Consumes one bounded script entry and rejects transport calls that
 * exceed the configured sequence.
 * @param[in] speed Negotiated USB bus speed supplied by the BOT state machine.
 * @param[in] pipe_num Host-controller pipe selected for the transfer.
 * @param[in] data Bytes submitted by the BOT state machine.
 * @param[in] len Number of submitted bytes.
 * @return Scripted bulk-OUT result.
 * @retval k_ra8_ok The scripted transfer completed successfully.
 * @retval k_ra8_err_hw_error No scripted entry remained.
 * @retval k_ra8_err_hw_timeout The scripted transfer timed out.
 * @pre ::s_out_count does not exceed ::k_t_script_cap.
 * @pre @p data references @p len readable bytes when @p len is nonzero.
 * @post ::s_out_index advances exactly once when an entry is consumed.
 * @post No hardware register or caller-owned buffer is modified.
 * @note File-local synchronous SIE model; it performs no hardware access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mock_bulk_out(ra8_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len)
{
  (void)speed;
  (void)pipe_num;
  (void)data;
  (void)len;
  if (s_out_index >= s_out_count) {
    return k_ra8_err_hw_error;
  }
  const ra8_err_t result = s_out_results[s_out_index];
  ++s_out_index;
  return result;
}

/**
 * @brief Return the next scripted bulk-IN result and payload.
 * @details Copies at most @p max_len bytes from the current bounded script
 * entry when its result is successful.
 * @param[in] speed Negotiated USB bus speed supplied by the BOT state machine.
 * @param[in] pipe_num Host-controller pipe selected for the transfer.
 * @param[out] buf Destination for the scripted payload.
 * @param[in] max_len Capacity of @p buf in bytes.
 * @param[out] out_received Number of bytes copied into @p buf.
 * @return Scripted bulk-IN result.
 * @retval k_ra8_ok The scripted payload was copied successfully.
 * @retval k_ra8_err_hw_error No scripted entry remained.
 * @retval k_ra8_err_hw_timeout The scripted transfer timed out.
 * @pre ::s_in_count does not exceed ::k_t_script_cap and successful entries
 * reference payload storage valid for their configured lengths.
 * @pre @p buf and @p out_received reference writable caller-owned storage.
 * @post ::s_in_index advances exactly once when an entry is consumed.
 * @post @p out_received reports zero on error or the copied payload length.
 * @note File-local synchronous SIE model; it performs no hardware access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mock_bulk_in(ra8_usb_speed_t speed,
                                                    uint8_t         pipe_num,
                                                    uint8_t*        buf,
                                                    uint16_t        max_len,
                                                    uint16_t*       out_received)
{
  (void)speed;
  (void)pipe_num;
  if (s_in_index >= s_in_count) {
    *out_received = 0U;
    return k_ra8_err_hw_error;
  }
  const uint8_t   index  = s_in_index;
  const ra8_err_t result = s_in_results[index];
  ++s_in_index;
  if (result != k_ra8_ok) {
    *out_received = 0U;
    return result;
  }
  uint16_t copy_len = s_in_lengths[index];
  if (copy_len > max_len) {
    copy_len = max_len;
  }
  for (uint16_t i = 0U; i < copy_len; ++i) {
    buf[i] = s_in_payloads[index][i];
  }
  *out_received = copy_len;
  return k_ra8_ok;
}

/**
 * @brief Reset the bounded BOT transport script and copied driver state.
 * @details Clears both direction queues and initializes an attached full-speed
 * host-MSC device with a deterministic first CBW tag.
 * @pre All script arrays have ::k_t_script_cap entries.
 * @pre The copied host-MSC state object is available.
 * @post Both script indices and counts are zero and the copied driver is ready.
 * @post The next CBW tag equals ::k_t_initial_tag.
 * @note File-local fixture setup; no ownership escapes this executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_reset_script(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_t_script_cap; ++i) {
    s_out_results[i] = k_ra8_err_hw_error;
    s_in_results[i]  = k_ra8_err_hw_error;
    s_in_payloads[i] = nullptr;
    s_in_lengths[i]  = 0U;
  }
  s_out_count                                         = 0U;
  s_out_index                                         = 0U;
  s_in_count                                          = 0U;
  s_in_index                                          = 0U;
  g_usb_hmsc_state_bot_cov                            = (ra8_usb_hmsc_state_t){};
  g_usb_hmsc_state_bot_cov.initialized                = true;
  g_usb_hmsc_state_bot_cov.attached                   = true;
  g_usb_hmsc_state_bot_cov.speed                      = k_ra8_usb_speed_fs;
  g_usb_hmsc_state_bot_cov.device.bulk_out_max_packet = 64U;
  g_usb_hmsc_state_bot_cov.next_cbw_tag               = (uint32_t)k_t_initial_tag;
}

/**
 * @brief Append one result to the bounded bulk-OUT script.
 * @details Records the result at the next free output-script slot.
 * @param[in] result Status the next bulk-OUT call must return.
 * @pre ::s_out_count is less than ::k_t_script_cap.
 * @pre The bounded output-script storage is available.
 * @post ::s_out_count is incremented and the result is retained in call order.
 * @post ::s_out_index is unchanged because appending does not consume an entry.
 * @note A failed capacity assertion terminates the focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_push_out_result(ra8_err_t result)
{
  TEST_ASSERT(s_out_count < (uint8_t)k_t_script_cap);
  s_out_results[s_out_count] = result;
  ++s_out_count;
}

/**
 * @brief Append one result and optional payload to the bulk-IN script.
 * @details Records the status, payload pointer, and payload length together at
 * the next free input-script slot.
 * @param[in] result Status the next bulk-IN call must return.
 * @param[in] payload Payload bytes for a successful result, or nullptr on error.
 * @param[in] len Number of bytes available through @p payload.
 * @pre ::s_in_count is less than ::k_t_script_cap.
 * @pre @p payload references @p len bytes for a successful nonempty entry.
 * @post ::s_in_count is incremented and the entry is retained in call order.
 * @post ::s_in_index is unchanged because appending does not consume an entry.
 * @note Payload storage remains owned by the calling test.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_push_in_result(ra8_err_t result, const uint8_t* payload, uint16_t len)
{
  TEST_ASSERT(s_in_count < (uint8_t)k_t_script_cap);
  s_in_results[s_in_count]  = result;
  s_in_payloads[s_in_count] = payload;
  s_in_lengths[s_in_count]  = len;
  ++s_in_count;
}

/**
 * @brief Build a valid passing CSW for the supplied BOT tag.
 * @details Writes the BOT signature, echoed tag, and successful status into a
 * zero-initialized 13-byte command-status wrapper.
 * @param[out] csw Destination with at least ::k_t_csw_len bytes.
 * @param[in] tag CBW tag that the CSW must echo.
 * @pre @p csw points to writable storage of ::k_t_csw_len bytes.
 * @pre @p csw does not overlap immutable fixture storage.
 * @post @p csw contains a valid passing command-status wrapper.
 * @post The CSW residue and all reserved bytes are zero.
 * @note The residue field remains zero.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_build_valid_csw(uint8_t* csw, uint32_t tag)
{
  internal_zero_bytes(csw, (uint16_t)k_t_csw_len);
  internal_pack_u32_le(k_ra8_hmsc_csw_signature, &csw[k_ra8_hmsc_csw_off_signature]);
  internal_pack_u32_le(tag, &csw[k_ra8_hmsc_csw_off_tag]);
  csw[k_ra8_hmsc_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_passed;
}

/**
 * @test internal_test_issue_cbw_builder_error
 * @brief Verify CBW-builder rejection is propagated before bulk transport.
 * @details Supplies the invalid zero CDB length and observes the production
 * issue-CBW helper return without consuming the transport script.
 * @pre The copied BOT state is initialized and attached.
 * @pre The bulk-OUT script contains no entries.
 * @post The invalid-argument result is returned unchanged.
 * @post The bulk-OUT script index remains zero.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_issue_cbw_builder_error(void)
{
  TEST_BEGIN("BOT issue-CBW propagates builder rejection");
  internal_reset_script();
  uint8_t  cdb[1] = {};
  uint32_t tag    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_issue_cbw(0U, 0U, true, cdb, 0U, &tag));
  TEST_ASSERT_EQ(0U, s_out_index);
  TEST_END("BOT issue-CBW propagates builder rejection");
}

/**
 * @test internal_test_read_csw_errors
 * @brief Verify CSW receive and decode errors are independently propagated.
 * @details Scripts a bulk-IN timeout, then a successful transfer containing a
 * malformed CSW, exercising both return-on-error outcomes and their opposites.
 * @pre Each scenario begins with the copied BOT state reset.
 * @pre Input payload storage remains valid until its scripted call completes.
 * @post The receive timeout is returned unchanged in the first scenario.
 * @post The malformed wrapper is rejected after successful transport.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_read_csw_errors(void)
{
  TEST_BEGIN("BOT CSW receive and decode errors propagate");
  internal_reset_script();
  internal_push_in_result(k_ra8_err_hw_timeout, nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, internal_read_csw((uint32_t)k_t_initial_tag));

  internal_reset_script();
  uint8_t malformed_csw[k_t_csw_len] = {};
  internal_push_in_result(k_ra8_ok, malformed_csw, (uint16_t)sizeof malformed_csw);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_read_csw((uint32_t)k_t_initial_tag));

  internal_reset_script();
  uint8_t short_csw[k_t_csw_len] = {};
  internal_push_in_result(k_ra8_ok, short_csw, (uint16_t)(sizeof short_csw - 1U));
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_read_csw((uint32_t)k_t_initial_tag));

  internal_reset_script();
  uint8_t failed_csw[k_t_csw_len] = {};
  internal_build_valid_csw(failed_csw, (uint32_t)k_t_initial_tag);
  failed_csw[k_ra8_hmsc_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_failed;
  internal_push_in_result(k_ra8_ok, failed_csw, (uint16_t)sizeof failed_csw);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_read_csw((uint32_t)k_t_initial_tag));
  TEST_END("BOT CSW receive and decode errors propagate");
}

/**
 * @test internal_test_data_in_errors
 * @brief Verify data-IN propagates CBW and data-stage failures independently.
 * @details Fails the initial bulk-OUT submission, then permits it and fails the
 * following bulk-IN payload transfer.
 * @pre Each scenario begins with the copied BOT state reset.
 * @pre The CDB and destination buffers remain valid for each synchronous call.
 * @post The CBW failure is returned before a data transfer is attempted.
 * @post The data-stage timeout is returned after a successful CBW submission.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_data_in_errors(void)
{
  TEST_BEGIN("BOT data-IN propagates CBW and data errors");
  uint8_t  cdb[k_t_cdb_len]            = {};
  uint8_t  data[k_t_small_payload_len] = {};
  uint16_t len                         = (uint16_t)sizeof data;

  internal_reset_script();
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_run_data_in(0U, cdb, (uint8_t)sizeof cdb, data, &len));

  internal_reset_script();
  internal_push_out_result(k_ra8_ok);
  internal_push_in_result(k_ra8_err_hw_timeout, nullptr, 0U);
  len = (uint16_t)sizeof data;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_run_data_in(0U, cdb, (uint8_t)sizeof cdb, data, &len));
  TEST_END("BOT data-IN propagates CBW and data errors");
}

/**
 * @test internal_test_data_out_paths
 * @brief Verify data-OUT CBW/data failures and a fully successful BOT tail.
 * @details Drives a CBW timeout, a data-chunk timeout, and a complete CBW,
 * payload, and valid-CSW sequence through the production state machine.
 * @pre Each scenario begins with the copied BOT state reset.
 * @pre CDB, data, and CSW buffers remain valid for each synchronous call.
 * @post Each selected transport failure is returned unchanged.
 * @post The complete scripted BOT sequence returns ::k_ra8_ok.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_data_out_paths(void)
{
  TEST_BEGIN("BOT data-OUT drives CBW, data, and CSW outcomes");
  uint8_t cdb[k_t_cdb_len]            = {};
  uint8_t data[k_t_small_payload_len] = {};
  uint8_t valid_csw[k_t_csw_len]      = {};

  internal_reset_script();
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_run_data_out(0U, cdb, (uint8_t)sizeof cdb, data, sizeof data));

  internal_reset_script();
  internal_push_out_result(k_ra8_ok);
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 internal_run_data_out(0U, cdb, (uint8_t)sizeof cdb, data, sizeof data));

  internal_reset_script();
  g_usb_hmsc_state_bot_cov.device.bulk_out_max_packet = 0U;
  internal_build_valid_csw(valid_csw, (uint32_t)k_t_initial_tag);
  internal_push_out_result(k_ra8_ok);
  internal_push_out_result(k_ra8_ok);
  internal_push_in_result(k_ra8_ok, valid_csw, (uint16_t)sizeof valid_csw);
  TEST_ASSERT_EQ(k_ra8_ok, internal_run_data_out(0U, cdb, (uint8_t)sizeof cdb, data, sizeof data));
  TEST_END("BOT data-OUT drives CBW, data, and CSW outcomes");
}

/**
 * @test internal_test_read_capacity_paths
 * @brief Verify READ CAPACITY propagates BOT failure and decodes success.
 * @details First fails CBW transport, then supplies a valid eight-byte capacity
 * payload and matching passing CSW through the synchronous script.
 * @pre Each scenario begins with the copied BOT state reset.
 * @pre Capacity and CSW payload storage remains valid during scripted reads.
 * @post The BOT failure is returned unchanged in the first scenario.
 * @post Success reports four 512-byte logical blocks in the second scenario.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_read_capacity_paths(void)
{
  TEST_BEGIN("READ CAPACITY propagates BOT failure and decodes success");
  uint32_t block_count = 0U;
  uint32_t block_size  = 0U;

  internal_reset_script();
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_usb_hmsc_read_capacity_bot_cov(0U, &block_count, &block_size));

  internal_reset_script();
  uint8_t capacity[k_t_data_len] = {0U, 0U, 0U, (uint8_t)k_t_last_lba, 0U, 0U, 2U, 0U};
  uint8_t valid_csw[k_t_csw_len] = {};
  internal_build_valid_csw(valid_csw, (uint32_t)k_t_initial_tag);
  internal_push_out_result(k_ra8_ok);
  internal_push_in_result(k_ra8_ok, capacity, (uint16_t)sizeof capacity);
  internal_push_in_result(k_ra8_ok, valid_csw, (uint16_t)sizeof valid_csw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_read_capacity_bot_cov(0U, &block_count, &block_size));
  TEST_ASSERT_EQ(k_t_block_count, block_count);
  TEST_ASSERT_EQ(k_t_block_size, block_size);

  internal_reset_script();
  uint8_t zero_capacity[k_t_data_len] = {};
  internal_build_valid_csw(valid_csw, (uint32_t)k_t_initial_tag);
  internal_push_out_result(k_ra8_ok);
  internal_push_in_result(k_ra8_ok, zero_capacity, (uint16_t)sizeof zero_capacity);
  internal_push_in_result(k_ra8_ok, valid_csw, (uint16_t)sizeof valid_csw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_read_capacity_bot_cov(0U, &block_count, &block_size));
  TEST_ASSERT_EQ(1U, block_count);
  TEST_ASSERT_EQ(k_t_block_size, block_size);
  TEST_END("READ CAPACITY propagates BOT failure and decodes success");
}

/**
 * @test internal_test_inquiry_paths
 * @brief Verify INQUIRY propagates BOT failure and decodes all fixed fields.
 * @details Drives one failed CBW submission and one complete INQUIRY data/CSW
 * exchange through the production command entry point.
 * @pre Each scenario begins with the copied BOT state reset.
 * @pre Script payloads remain valid until their synchronous calls complete.
 * @post The failed transport status is returned unchanged.
 * @post The successful response preserves qualifier, type, removable, version,
 * vendor, product, and revision bytes from the wire payload.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_inquiry_paths(void)
{
  TEST_BEGIN("INQUIRY propagates failure and decodes success");
  ra8_usb_hmsc_inquiry_response_t response = {};

  internal_reset_script();
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_hmsc_inquiry_bot_cov(0U, &response));

  internal_reset_script();
  uint8_t inquiry[k_ra8_hmsc_inquiry_resp_len] = {};
  inquiry[k_ra8_hmsc_inq_off_dev_type]         = 0x65U;
  inquiry[k_ra8_hmsc_inq_off_removable]        = 0x80U;
  inquiry[k_ra8_hmsc_inq_off_version]          = 0x06U;
  inquiry[k_ra8_hmsc_inq_off_vendor_id]        = (uint8_t)'R';
  inquiry[k_ra8_hmsc_inq_off_vendor_id + 7U]   = (uint8_t)'8';
  inquiry[k_ra8_hmsc_inq_off_product_id]       = (uint8_t)'B';
  inquiry[k_ra8_hmsc_inq_off_product_id + 15U] = (uint8_t)'K';
  inquiry[k_ra8_hmsc_inq_off_product_rev]      = (uint8_t)'0';
  inquiry[k_ra8_hmsc_inq_off_product_rev + 3U] = (uint8_t)'1';
  uint8_t valid_csw[k_t_csw_len]               = {};
  internal_build_valid_csw(valid_csw, (uint32_t)k_t_initial_tag);
  internal_push_out_result(k_ra8_ok);
  internal_push_in_result(k_ra8_ok, inquiry, (uint16_t)sizeof inquiry);
  internal_push_in_result(k_ra8_ok, valid_csw, (uint16_t)sizeof valid_csw);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_inquiry_bot_cov(0U, &response));
  TEST_ASSERT_EQ(3U, response.peripheral_qualifier);
  TEST_ASSERT_EQ(5U, response.peripheral_device_type);
  TEST_ASSERT_EQ(1U, response.removable);
  TEST_ASSERT_EQ(6U, response.version);
  TEST_ASSERT_EQ('R', response.vendor_id[0]);
  TEST_ASSERT_EQ('8', response.vendor_id[7]);
  TEST_ASSERT_EQ('B', response.product_id[0]);
  TEST_ASSERT_EQ('K', response.product_id[15]);
  TEST_ASSERT_EQ('0', response.product_revision[0]);
  TEST_ASSERT_EQ('1', response.product_revision[3]);
  TEST_END("INQUIRY propagates failure and decodes success");
}

/**
 * @test internal_test_read10_paths
 * @brief Verify READ(10) propagates data failure and completes a BOT read.
 * @details Scripts a successful CBW followed by a failed data stage, then a
 * complete CBW/data/CSW exchange for a one-block read.
 * @pre The copied host-MSC state is initialized and attached.
 * @pre The destination buffer has one logical block of writable storage.
 * @post The data-stage failure is returned unchanged.
 * @post A complete exchange returns ::k_ra8_ok and copies its payload.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_read10_paths(void)
{
  TEST_BEGIN("READ(10) propagates data failure and completes");
  uint8_t block[k_ra8_hmsc_block_size_default] = {};

  internal_reset_script();
  internal_push_out_result(k_ra8_ok);
  internal_push_in_result(k_ra8_err_hw_timeout, nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_hmsc_read10_bot_cov(0U, 7U, 1U, block));

  internal_reset_script();
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_hmsc_read10_bot_cov(0U, 7U, UINT16_MAX, block));

  internal_reset_script();
  const uint8_t payload[k_t_small_payload_len] = {1U, 2U, 3U, 4U};
  uint8_t       valid_csw[k_t_csw_len]         = {};
  internal_build_valid_csw(valid_csw, (uint32_t)k_t_initial_tag);
  internal_push_out_result(k_ra8_ok);
  internal_push_in_result(k_ra8_ok, payload, (uint16_t)sizeof payload);
  internal_push_in_result(k_ra8_ok, valid_csw, (uint16_t)sizeof valid_csw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_read10_bot_cov(0U, 7U, 1U, block));
  TEST_ASSERT_EQ(payload[0], block[0]);
  TEST_ASSERT_EQ(payload[3], block[3]);
  TEST_END("READ(10) propagates data failure and completes");
}

/**
 * @test internal_test_write10_paths
 * @brief Verify WRITE(10) propagates data failure and completes a BOT write.
 * @details Scripts a successful CBW followed by a failed data packet, then a
 * complete one-block transfer split across the enumerated 64-byte packets and
 * closed by a passing CSW.
 * @pre The copied host-MSC state is initialized and attached at Full-Speed.
 * @pre The input buffer contains one readable logical block.
 * @post The data-stage failure is returned unchanged.
 * @post A complete exchange consumes one CBW and eight data packets.
 * @note File-local host test; no USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_write10_paths(void)
{
  TEST_BEGIN("WRITE(10) propagates data failure and completes");
  uint8_t block[k_ra8_hmsc_block_size_default] = {};

  internal_reset_script();
  internal_push_out_result(k_ra8_ok);
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_hmsc_write10_bot_cov(0U, 9U, 1U, block));

  internal_reset_script();
  internal_push_out_result(k_ra8_err_hw_timeout);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_usb_hmsc_write10_bot_cov(0U, 9U, UINT16_MAX, block));

  internal_reset_script();
  uint8_t valid_csw[k_t_csw_len] = {};
  internal_build_valid_csw(valid_csw, (uint32_t)k_t_initial_tag);
  for (uint8_t i = 0U; i < 9U; ++i) {
    internal_push_out_result(k_ra8_ok);
  }
  internal_push_in_result(k_ra8_ok, valid_csw, (uint16_t)sizeof valid_csw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_write10_bot_cov(0U, 9U, 1U, block));
  TEST_ASSERT_EQ(9U, s_out_index);
  TEST_END("WRITE(10) propagates data failure and completes");
}

/**
 * @test internal_test_private_copy_mcdc
 * @brief Cover the compound BOT decisions in this private host-MSC copy.
 * @details Drives the speed envelope, CDB-length envelope, and three-condition
 * CSW status chain in the copy included by this white-box executable.
 * @pre Off-target USB register fakes are active.
 * @post The copied BOT state is reset before returning.
 * @note No USB controller or hardware is touched.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_private_copy_mcdc(void)
{
  TEST_BEGIN("host MSC private copy has complete MC/DC vectors");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init_bot_cov(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_close_bot_cov());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_init_bot_cov(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_close_bot_cov());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_hmsc_init_bot_cov((ra8_usb_speed_t)9U));
  TEST_ASSERT_EQ(k_ra8_hmsc_bulk_max_packet_fs, internal_bulk_max_packet(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_hmsc_bulk_max_packet_hs, internal_bulk_max_packet(k_ra8_usb_speed_hs));

  const uint8_t cdb[k_t_cdb_len]        = {0x12U, 0U, 0U, 0U, 0U, 0U};
  uint8_t       cbw[k_ra8_hmsc_cbw_len] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_build_cbw_bot_cov(0U, 0U, false, cdb, k_t_cdb_len, cbw));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hmsc_build_cbw_bot_cov(0U, 0U, false, cdb, 0U, cbw));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hmsc_build_cbw_bot_cov(0U,
                                                0U,
                                                false,
                                                cdb,
                                                (uint8_t)(k_ra8_hmsc_cdb_max_len + 1U),
                                                cbw));

  const uint32_t expected_tag            = (uint32_t)k_t_initial_tag;
  uint8_t        csw[k_ra8_hmsc_csw_len] = {};
  internal_pack_u32_le(k_ra8_hmsc_csw_signature, &csw[k_ra8_hmsc_csw_off_signature]);
  internal_pack_u32_le(expected_tag, &csw[k_ra8_hmsc_csw_off_tag]);
  ra8_usb_hmsc_csw_status_t status = k_ra8_hmsc_csw_status_passed;
  csw[k_ra8_hmsc_csw_off_status]   = (uint8_t)k_ra8_hmsc_csw_status_passed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw_bot_cov(csw, expected_tag, &status));
  csw[k_ra8_hmsc_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_failed;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw_bot_cov(csw, expected_tag, &status));
  csw[k_ra8_hmsc_csw_off_status] = (uint8_t)k_ra8_hmsc_csw_status_phase_error;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_decode_csw_bot_cov(csw, expected_tag, &status));
  csw[k_ra8_hmsc_csw_off_status] = 0xFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_hmsc_decode_csw_bot_cov(csw, expected_tag, &status));
  internal_reset_script();
  TEST_END("host MSC private copy has complete MC/DC vectors");
}

int main(void)
{
  internal_test_issue_cbw_builder_error();
  internal_test_read_csw_errors();
  internal_test_data_in_errors();
  internal_test_data_out_paths();
  internal_test_read_capacity_paths();
  internal_test_inquiry_paths();
  internal_test_read10_paths();
  internal_test_write10_paths();
  internal_test_private_copy_mcdc();
  return 0;
}
