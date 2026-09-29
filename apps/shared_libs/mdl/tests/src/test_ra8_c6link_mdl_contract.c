/**
 * @file test_ra8_c6link_mdl_contract.c
 * @brief Binding vectors for the published media dispatch contract type
 * @details ::ra8_mdl_service_dispatch_fn is the contract an RPC integration
 * writes against, and nothing in the tree held ::ra8_mdl_service_dispatch to
 * it: the ESP-hosted hook calls the function by name, so an argument change
 * kept every build green. Every call in this file goes through a
 * ::ra8_mdl_service_dispatch_fn value instead, and the status set the contract
 * documents is executed one code at a time.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_c6link_mdl_service_internal.h"
#include "ra8_err.h"
#include "ra8_mdl_protocol.h"
#include "ra8_media_download.pb-c.h"
#include "test_ra8_c6link_mdl_contract_internal.h"
#include "test_ra8_c6link_mdl_policy_internal.h"
#include "unity_minimal.h"

/** @enum t_contract_const_t @brief Fixed capacities and operands of this fixture. */
typedef enum : uint16_t {
  k_t_contract_request_bytes  = 700U,  /**< Packed request scratch capacity.      */
  k_t_contract_response_bytes = 1200U, /**< Packed response scratch capacity.     */
  k_t_contract_pull_bytes     = 4U,    /**< Bounded body bytes one Next pulls.    */
  k_t_contract_len_sentinel   = 77U,   /**< Non-zero output-length sentinel.      */
  k_t_contract_unowned_op     = 0U,    /**< Operation id the service disowns.     */
  k_t_contract_probe_len      = 1U,    /**< Request length for a rejected call.   */
} t_contract_const_t;

static ra8_test_mdl_backend_t s_backend;
static ra8_mdl_service_t      s_service;
static uint8_t                s_request[k_t_contract_request_bytes];
static uint8_t                s_response[k_t_contract_response_bytes];

/** @brief The published contract, bound to its only implementation. */
static const ra8_mdl_service_dispatch_fn s_dispatch = ra8_mdl_service_dispatch;

/* The assignment above already rejects an incompatible implementation. This
 * pins the stronger property the contract needs: the implementation has
 * exactly the published type, with no compatible-but-different spelling. */
static_assert(_Generic(&ra8_mdl_service_dispatch,
                       ra8_mdl_service_dispatch_fn: 1,
                       default: 0) == 1,
              "ra8_mdl_service_dispatch must have exactly ra8_mdl_service_dispatch_fn");

/** @brief Rebind a reset service over the deterministic fixture backend.
 * @details Implements the reset fixture operation used only by this focused
 * runner. @pre Fixed-capacity fixture storage required by this operation is
 * available. @post The service is initialised, inactive, and ready to
 * dispatch. @note File-local helper; no ownership escapes this executable.
 * @since Version 0.1.0 */
RA8_INTERNAL static void internal_contract_reset(void)
{
  static const uint8_t bytes[] = {'a', 'b', 'c', 'd', 'e', 'f'};
  s_backend                    = (ra8_test_mdl_backend_t){.bytes = bytes, .len = sizeof(bytes)};
  const ra8_mdl_service_backend_t backend = {.begin  = priv_test_mdl_backend_begin,
                                             .read   = priv_test_mdl_backend_read,
                                             .cancel = priv_test_mdl_backend_cancel,
                                             .ctx    = &s_backend};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_service_init(&s_service, &backend));
}

/** @brief Pack the fixture Start request into the scratch buffer.
 * @details Implements the Start encoding fixture operation used only by this
 * focused runner. @return Packed request length. @retval value Bytes written
 * to the fixture request scratch. @pre Fixed-capacity fixture storage required
 * by this operation is available. @post The scratch holds one decodable Start.
 * @note File-local helper; no ownership escapes this executable. @since
 * Version 0.1.0 */
RA8_INTERNAL static size_t internal_contract_pack_start(void)
{
  Ra8__Mdl__StartRequest req = RA8__MDL__START_REQUEST__INIT;
  req.protocol_version       = k_ra8_mdl_protocol_version;
  req.url                    = (char*)"https://example.test/book";
  req.format                 = RA8__MDL__FORMAT__FORMAT_RABOOK;
  return ra8__mdl__start_request__pack(&req, s_request);
}

/** @brief Pack one Next request into the scratch buffer.
 * @details Implements the Next encoding fixture operation used only by this
 * focused runner. @param[in] job Job identity the pull correlates against.
 * @param[in] offset Acknowledged body offset. @return Packed request length.
 * @retval value Bytes written to the fixture request scratch. @pre
 * Fixed-capacity fixture storage required by this operation is available.
 * @post The scratch holds one decodable Next. @note File-local helper; no
 * ownership escapes this executable. @since Version 0.1.0 */
RA8_INTERNAL static size_t internal_contract_pack_next(uint32_t job, uint64_t offset)
{
  Ra8__Mdl__NextRequest req = RA8__MDL__NEXT_REQUEST__INIT;
  req.protocol_version      = k_ra8_mdl_protocol_version;
  req.job_id                = job;
  req.acknowledged_offset   = offset;
  req.max_bytes             = k_t_contract_pull_bytes;
  return ra8__mdl__next_request__pack(&req, s_request);
}

/** @brief Pack one Cancel request into the scratch buffer.
 * @details Implements the Cancel encoding fixture operation used only by this
 * focused runner. @param[in] job Job identity to cancel. @return Packed
 * request length. @retval value Bytes written to the fixture request scratch.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @post The scratch holds one decodable Cancel. @note File-local helper; no
 * ownership escapes this executable. @since Version 0.1.0 */
RA8_INTERNAL static size_t internal_contract_pack_cancel(uint32_t job)
{
  Ra8__Mdl__CancelRequest req = RA8__MDL__CANCEL_REQUEST__INIT;
  req.protocol_version        = k_ra8_mdl_protocol_version;
  req.job_id                  = job;
  return ra8__mdl__cancel_request__pack(&req, s_request);
}

/** @brief Run one operation through the contract value.
 * @details Implements the indirect dispatch fixture operation used only by
 * this focused runner, so no vector here names the implementation.
 * @param[in] operation Operation id to dispatch. @param[in] request_len Packed
 * request length. @param[out] response_len Packed response length.
 * @return Dispatch status from the contract. @retval value The status the
 * contract returned for this call. @pre Fixed-capacity fixture storage
 * required by this operation is available. @post The response scratch holds
 * the packed response on success. @note File-local helper; no ownership
 * escapes this executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_contract_call(uint32_t operation, size_t request_len, size_t* response_len)
{
  return s_dispatch(&s_service,
                    operation,
                    s_request,
                    request_len,
                    s_response,
                    sizeof(s_response),
                    response_len);
}

/**
 * @brief Drive a Start, Next, and Cancel sequence through the contract value
 * @details Every operation is invoked through ::ra8_mdl_service_dispatch_fn,
 * and each response is decoded with the generated codec, so the contract is
 * proven usable end to end rather than merely non-null.
 * @pre The unity-minimal assertion process is initialized.
 * @post The job started here is cancelled and the service is inactive.
 * @note File-local vector; no ownership escapes this executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_contract_round_trip(void)
{
  TEST_BEGIN("mdl dispatch contract round trip");
  internal_contract_reset();
  TEST_ASSERT(s_dispatch != nullptr);

  size_t response_len = k_t_contract_len_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_contract_call(k_ra8_mdl_rpc_start,
                                        internal_contract_pack_start(),
                                        &response_len));
  Ra8__Mdl__Accepted* accepted = ra8__mdl__accepted__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(accepted != nullptr);
  TEST_ASSERT_EQ(k_ra8_mdl_protocol_version, accepted->protocol_version);
  TEST_ASSERT(accepted->job_id != 0U);
  const uint32_t job = accepted->job_id;
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);
  TEST_ASSERT_EQ(1, s_backend.begins);

  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_contract_call(k_ra8_mdl_rpc_next,
                                        internal_contract_pack_next(job, 0U),
                                        &response_len));
  Ra8__Mdl__Chunk* chunk = ra8__mdl__chunk__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(chunk != nullptr);
  TEST_ASSERT_EQ(job, chunk->job_id);
  TEST_ASSERT_EQ(0, chunk->sequence);
  TEST_ASSERT_EQ(0, chunk->offset);
  TEST_ASSERT_EQ(k_t_contract_pull_bytes, chunk->data.len);
  TEST_ASSERT(memcmp(chunk->data.data, "abcd", k_t_contract_pull_bytes) == 0);
  TEST_ASSERT_EQ(RA8__MDL__STATE__STATE_DOWNLOADING, chunk->state);
  ra8__mdl__chunk__free_unpacked(chunk, nullptr);

  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_contract_call(k_ra8_mdl_rpc_cancel,
                                        internal_contract_pack_cancel(job),
                                        &response_len));
  Ra8__Mdl__Cancelled* ack = ra8__mdl__cancelled__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(ack != nullptr);
  TEST_ASSERT_EQ(job, ack->job_id);
  ra8__mdl__cancelled__free_unpacked(ack, nullptr);
  TEST_ASSERT_EQ(1, s_backend.cancels);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl dispatch contract round trip");
}

/**
 * @brief Execute the status set the contract documents for a rejected call
 * @details One vector per code, each through the contract value: an operation
 * the service does not own, a null service context, a Start arriving while a
 * job is active, and a Next naming a job the service never issued. A rejected
 * call must also leave the published length output at zero.
 * @pre The unity-minimal assertion process is initialized.
 * @post The job started here is cancelled and the service is inactive.
 * @note File-local vector; no ownership escapes this executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_contract_status_set(void)
{
  TEST_BEGIN("mdl dispatch contract status set");
  internal_contract_reset();

  size_t response_len = k_t_contract_len_sentinel;
  s_request[0]        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 internal_contract_call(k_t_contract_unowned_op,
                                        k_t_contract_probe_len,
                                        &response_len));
  TEST_ASSERT_EQ(0, response_len);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 s_dispatch(nullptr,
                            k_t_contract_unowned_op,
                            s_request,
                            k_t_contract_probe_len,
                            s_response,
                            sizeof(s_response),
                            &response_len));

  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_contract_call(k_ra8_mdl_rpc_start,
                                        internal_contract_pack_start(),
                                        &response_len));
  Ra8__Mdl__Accepted* accepted = ra8__mdl__accepted__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(accepted != nullptr);
  const uint32_t job = accepted->job_id;
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);

  TEST_ASSERT_EQ(k_ra8_err_busy,
                 internal_contract_call(k_ra8_mdl_rpc_start,
                                        internal_contract_pack_start(),
                                        &response_len));
  TEST_ASSERT_EQ(1, s_backend.begins);

  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 internal_contract_call(k_ra8_mdl_rpc_next,
                                        internal_contract_pack_next(job + 1U, 0U),
                                        &response_len));

  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_contract_call(k_ra8_mdl_rpc_cancel,
                                        internal_contract_pack_cancel(job),
                                        &response_len));
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl dispatch contract status set");
}

void priv_test_c6link_mdl_contract_run(void)
{
  internal_test_contract_round_trip();
  internal_test_contract_status_set();
}
