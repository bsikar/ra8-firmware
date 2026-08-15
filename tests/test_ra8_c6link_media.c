/**
 * @file test_ra8_c6link_media.c
 * @brief Media-download protocol and transactional-transfer facade tests.
 *
 * @details
 * Exercises the downloader-specific C6 link RPC path against the protocol-aware
 * co-processor model, then qualifies the bounded transfer coordinator with
 * caller-owned storage and digest seams. The generic C6 link facade vectors
 * remain in `test_ra8_c6link.c`; both suites share only the explicit model
 * fixture declared by `ra8_c6link_model_test_internal.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_c6_model.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_mdl_transfer.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @enum internal_media_const_t @brief Media fixture constants. */
typedef enum : uint8_t {
  k_internal_mdl_digest_fill     = 0xA5U, /**< Expected model digest octet.   */
  k_internal_mdl_bad_digest_fill = 0x5AU, /**< Deliberately mismatched octet. */
} internal_media_const_t;

/**
 * @brief Exercise both protobuf layers and the full modelled transport path.
 * @par MC/DC:
 * The terminal-state decision
 * `(state == COMPLETE) || (state == CANCELLED) || (state == FAILED)` executes
 * V1 DOWNLOADING -> F,F,F/false for the two data chunks and
 * V2 COMPLETE -> T,-,-/true for the digest-bearing terminal chunk. V1/V2
 * independently vary COMPLETE. The COMPLETE semantic conjunction executes
 * its all-true control (zero data, 32-byte non-null digest, status zero, and
 * total equal to end); the explicit cancel then exercises the separately
 * correlated cancellation-acknowledgement path.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_chunk_semantics_valid
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_take_chunk
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_take_cancelled @details
 * Executes the media download roundtrip scenario with bounded fixture state and
 * asserts the contract-specific result. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_media_download_roundtrip(void)
{
  TEST_BEGIN("c6link media download roundtrip");
  priv_c6link_test_bringup();

  ra8_mdl_session_t session = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_mdl_start(priv_c6link_test_link(), "https://example.test/book", &session));
  TEST_ASSERT(session.active);
  TEST_ASSERT(session.job_id != 0U);

  uint8_t received[sizeof "abcdef" - 1U] = {};
  size_t  received_len                   = 0U;
  while (session.active) {
    ra8_mdl_chunk_t chunk = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 4U, &chunk));
    TEST_ASSERT((received_len + chunk.data_len) <= sizeof(received));
    (void)memcpy(&received[received_len], chunk.data, chunk.data_len);
    received_len += chunk.data_len;
    if (!session.active) {
      TEST_ASSERT(chunk.has_sha256);
      TEST_ASSERT_EQ((int64_t)0xA5, (int64_t)chunk.sha256[0]);
    }
  }
  TEST_ASSERT_EQ((int64_t)sizeof(received), (int64_t)received_len);
  TEST_ASSERT(memcmp(received, "abcdef", sizeof(received)) == 0);

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_mdl_start(priv_c6link_test_link(), "https://example.test/book", &session));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_cancel(priv_c6link_test_link(), &session));
  TEST_ASSERT(!session.active);
  TEST_END("c6link media download roundtrip");
}

/** @brief Start a modelled job and consume its one non-terminal data frame.
 * @details Implements the mdl before terminal fixture operation used only by
 * this focused test executable. @param[in,out] session Fixture argument
 * governed by the exercised interface contract. @pre Fixed-capacity fixture
 * storage required by this operation is available. @pre Arguments follow the
 * interface contract exercised by this helper. @post Documented outputs contain
 * the exercised result when the operation succeeds. @post Mutations remain
 * confined to documented outputs and file-local fixture state. @note File-local
 * helper; no ownership escapes this focused test executable. @since Version
 * 0.1.0 */
RA8_INTERNAL
static void internal_mdl_before_terminal(ra8_mdl_session_t* session)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_mdl_start(priv_c6link_test_link(), "https://example.test/book", session));
  ra8_mdl_chunk_t data = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_next(priv_c6link_test_link(), session, 6U, &data));
  TEST_ASSERT_EQ((int64_t)6, (int64_t)data.data_len);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_state_downloading, (int64_t)data.state);
  TEST_ASSERT(session->active);
  TEST_ASSERT_EQ((int64_t)6, (int64_t)session->next_offset);
}

/**
 * @brief Reject malformed terminal metadata without advancing the session.
 * @par MC/DC:
 * Each fault changes one state-specific condition from a coherent companion
 * vector while correlation, sequence, offset, and requested length stay valid:
 * V1 COMPLETE without SHA makes `sha256.len==32` false (the all-true control is
 * in `internal_test_media_download_roundtrip`); V2 COMPLETE with total=end+1
 * changes
 * `(total==0U) || (total==end)` from F,T/true to F,F/false; V3 FAILED with
 * status zero changes `status>0` from true to false (control in
 * `internal_test_media_download_terminal_status`); V4 DOWNLOADING with nonzero
 * status changes `status==0` from true to false against the valid data frame
 * this test first consumes; V5 CANCELLED with data changes `data.len==0` from
 * true to false (control in `internal_test_media_download_terminal_status`).
 * Every rejected vector preserves the active session and its offset/sequence.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_chunk_semantics_valid
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_take_chunk @details
 * Executes the media download rejects bad terminal frames scenario with bounded
 * fixture state and asserts the contract-specific result. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_media_download_rejects_bad_terminal_frames(void)
{
  TEST_BEGIN("c6link media rejects bad terminal frames");

  priv_c6link_test_bringup();
  ra8_mdl_session_t session = {};
  internal_mdl_before_terminal(&session);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_complete_no_sha;
  ra8_mdl_chunk_t chunk     = {};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ((int64_t)6, (int64_t)session.next_offset);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)session.next_sequence);

  priv_c6link_test_bringup();
  session = (ra8_mdl_session_t){};
  internal_mdl_before_terminal(&session);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_complete_bad_total;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ((int64_t)6, (int64_t)session.next_offset);

  priv_c6link_test_bringup();
  session = (ra8_mdl_session_t){};
  internal_mdl_before_terminal(&session);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_failed_zero_status;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(session.active);

  priv_c6link_test_bringup();
  session = (ra8_mdl_session_t){};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_mdl_start(priv_c6link_test_link(), "https://example.test/book", &session));
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_downloading_error;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)session.next_offset);

  priv_c6link_test_bringup();
  session = (ra8_mdl_session_t){};
  internal_mdl_before_terminal(&session);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_cancelled_with_data;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(session.active);

  TEST_END("c6link media rejects bad terminal frames");
}

/**
 * @brief Accept coherent terminal failure/cancellation with explicit semantics.
 * @par MC/DC:
 * The terminal-state decision
 * `(state == COMPLETE) || (state == CANCELLED) || (state == FAILED)` executes
 * V1 DOWNLOADING -> F,F,F/false before each terminal,
 * V2 FAILED -> F,F,T/true, and V3 CANCELLED -> F,T,-/true. V1/V2
 * independently vary FAILED and V1/V3 independently vary CANCELLED. The
 * state-specific semantic conjunctions execute their all-true controls:
 * FAILED has a positive bounded status with no data/digest, while CANCELLED
 * has zero status with no data/digest.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_chunk_semantics_valid
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_take_chunk @details
 * Executes the media download terminal status scenario with bounded fixture
 * state and asserts the contract-specific result. @pre Fixed-capacity fixture
 * storage required by this operation is available. @pre Arguments follow the
 * interface contract exercised by this helper. @post Documented outputs contain
 * the exercised result when the operation succeeds. @post Mutations remain
 * confined to documented outputs and file-local fixture state. @note File-local
 * helper; no ownership escapes this focused test executable. @since Version
 * 0.1.0 */
RA8_INTERNAL
static void internal_test_media_download_terminal_status(void)
{
  TEST_BEGIN("c6link media terminal status");

  priv_c6link_test_bringup();
  ra8_mdl_session_t session = {};
  internal_mdl_before_terminal(&session);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_failed;
  ra8_mdl_chunk_t chunk     = {};
  TEST_ASSERT_EQ(k_ra8_fail, ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(!session.active);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_state_failed, (int64_t)chunk.state);
  TEST_ASSERT_EQ((int64_t)k_ra8_fail, (int64_t)chunk.status);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)chunk.data_len);
  TEST_ASSERT(!chunk.has_sha256);

  priv_c6link_test_bringup();
  session = (ra8_mdl_session_t){};
  internal_mdl_before_terminal(&session);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_cancelled;
  chunk                     = (ra8_mdl_chunk_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 6U, &chunk));
  TEST_ASSERT(!session.active);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_state_cancelled, (int64_t)chunk.state);
  TEST_ASSERT_EQ((int64_t)k_ra8_ok, (int64_t)chunk.status);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)chunk.data_len);
  TEST_ASSERT(!chunk.has_sha256);

  TEST_END("c6link media terminal status");
}

/**
 * @test internal_test_media_download_rejects_unknown_response_fields
 * @brief Reject unknown fields in every media response without local state
 * drift.
 * @details Injects an unknown protobuf varint into Accepted, Chunk, and
 * Cancelled messages after the service has packed otherwise canonical
 * responses.
 * @pre Each model bring-up resets the portable service and response-fault slot.
 * @post Rejected responses do not activate, advance, or deactivate the caller's
 *       session; a canonical cancel still recovers the rejected Chunk session.
 * @note Remote service effects may already have occurred before response
 * decode.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_media_download_rejects_unknown_response_fields(void)
{
  TEST_BEGIN("c6link media rejects unknown response fields");
  priv_c6link_test_bringup();
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_unknown_field;
  ra8_mdl_session_t session = {};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    ra8_c6link_mdl_start(priv_c6link_test_link(), "https://example.test/book", &session));
  TEST_ASSERT(!session.active);

  priv_c6link_test_bringup();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_mdl_start(priv_c6link_test_link(), "https://example.test/book", &session));
  const uint32_t job        = session.job_id;
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_unknown_field;
  ra8_mdl_chunk_t chunk     = {};
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_next(priv_c6link_test_link(), &session, 4U, &chunk));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ((int64_t)job, (int64_t)session.job_id);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)session.next_offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)session.next_sequence);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_mdl_cancel(priv_c6link_test_link(), &session));

  priv_c6link_test_bringup();
  session = (ra8_mdl_session_t){};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_mdl_start(priv_c6link_test_link(), "https://example.test/book", &session));
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_unknown_field;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_c6link_mdl_cancel(priv_c6link_test_link(), &session));
  TEST_ASSERT(session.active);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)session.next_offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)session.next_sequence);
  TEST_END("c6link media rejects unknown response fields");
}

/** @brief Failure injection and observations for a transactional store. */
typedef struct {
  uint8_t   bytes[16];        /**< Bytes written to temporary storage.  */
  uint16_t  len;              /**< Number of valid stored bytes.        */
  ra8_err_t write_error;      /**< Injected write result.               */
  ra8_err_t validation_error; /**< Injected artifact validation result. */
  bool      short_write;      /**< Whether writes accept one less byte. */
  uint8_t   begins;           /**< Begin callback count.                */
  uint8_t   validations;      /**< Validation callback count.           */
  uint8_t   commits;          /**< Commit callback count.               */
  uint8_t   aborts;           /**< Abort callback count.                */
} internal_mdl_store_t;

/** @brief Failure injection and observations for a streaming hash. */
typedef struct {
  uint8_t  bytes[16];  /**< Bytes observed by the model hash. */
  uint16_t len;        /**< Number of observed hash bytes.    */
  bool     bad_digest; /**< Whether final emits corruption.   */
} internal_mdl_hash_t;

/** @brief One fixed set of coordinator seams. */
typedef struct {
  internal_mdl_store_t store;  /**< Transactional storage fixture. */
  internal_mdl_hash_t  hash;   /**< Streaming hash fixture.        */
  bool                 cancel; /**< Cooperative cancellation flag. */
} internal_mdl_transfer_fixture_t;

static internal_mdl_transfer_fixture_t s_mdl_transfer;

/** @brief Begin a modelled temporary object. @details Implements the mdl store
 * begin fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[out] destination Fixture argument governed by the exercised
 * interface contract. @return RA8 status from the exercised fixture operation.
 * @retval k_ra8_ok The fixture operation completed successfully. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_store_begin(void* ctx, const char* destination)
{
  internal_mdl_store_t* store = (internal_mdl_store_t*)ctx;
  if ((store == nullptr) || (strcmp(destination, "/library/book.rabook") != 0)) {
    return k_ra8_err_invalid_arg;
  }
  store->begins += 1U;
  store->len = 0U;
  return k_ra8_ok;
}

/** @brief Append to modelled temporary storage with short/full injection.
 * @details Implements the mdl store write fixture operation used only by this
 * focused test executable. @param[in,out] ctx Fixture argument governed by the
 * exercised interface contract. @param[in] data Fixture argument governed by
 * the exercised interface contract. @param[in] len Fixture argument governed by
 * the exercised interface contract. @param[in,out] written Fixture argument
 * governed by the exercised interface contract. @return RA8 status from the
 * exercised fixture operation. @retval k_ra8_ok The fixture operation completed
 * successfully. @pre Fixed-capacity fixture storage required by this operation
 * is available. @pre Arguments follow the interface contract exercised by this
 * helper. @post Documented outputs contain the exercised result when the
 * operation succeeds. @post Mutations remain confined to documented outputs and
 * file-local fixture state. @note File-local helper; no ownership escapes this
 * focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t
internal_mdl_store_write(void* ctx, const uint8_t* data, uint16_t len, uint16_t* written)
{
  internal_mdl_store_t* store = (internal_mdl_store_t*)ctx;
  *written                    = 0U;
  if (store->write_error != k_ra8_ok) {
    return store->write_error;
  }
  uint16_t accepted = store->short_write ? (uint16_t)(len - 1U) : len;
  if ((uint32_t)store->len + accepted > sizeof(store->bytes)) {
    return k_ra8_err_no_mem;
  }
  memcpy(&store->bytes[store->len], data, accepted);
  store->len += accepted;
  *written = accepted;
  return k_ra8_ok;
}

/** @brief Validate the model artifact signature before publication. @details
 * Implements the mdl store validate fixture operation used only by this focused
 * test executable. @param[in,out] ctx Fixture argument governed by the
 * exercised interface contract. @param[in] total_bytes Fixture argument
 * governed by the exercised interface contract. @param[in] sha256 Fixture
 * argument governed by the exercised interface contract. @return RA8 status
 * from the exercised fixture operation. @retval k_ra8_ok The fixture operation
 * completed successfully. @pre Fixed-capacity fixture storage required by this
 * operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_store_validate(void*         ctx,
                                             uint64_t      total_bytes,
                                             const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  internal_mdl_store_t* store = (internal_mdl_store_t*)ctx;
  store->validations += 1U;
  if (store->validation_error != k_ra8_ok) {
    return store->validation_error;
  }
  if ((total_bytes != 6U) || (store->len != 6U) || (memcmp(store->bytes, "abcdef", 6U) != 0) ||
      (sha256[0] != k_internal_mdl_digest_fill)) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/** @brief Publish the modelled object. @details Implements the mdl store commit
 * fixture operation used only by this focused test executable. @param[in,out]
 * ctx Fixture argument governed by the exercised interface contract. @return
 * RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture
 * operation completed successfully. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_store_commit(void* ctx)
{
  internal_mdl_store_t* store = (internal_mdl_store_t*)ctx;
  store->commits += 1U;
  return k_ra8_ok;
}

/** @brief Destroy the modelled temporary object. @details Implements the mdl
 * store abort fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @return RA8 status from the exercised fixture operation. @retval
 * k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_store_abort(void* ctx)
{
  internal_mdl_store_t* store = (internal_mdl_store_t*)ctx;
  store->aborts += 1U;
  store->len = 0U;
  return k_ra8_ok;
}

/** @brief Reset the modelled SHA-256 context. @details Implements the mdl sha
 * init fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @return RA8 status from the exercised fixture operation. @retval
 * k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_sha_init(void* ctx)
{
  internal_mdl_hash_t* hash = (internal_mdl_hash_t*)ctx;
  hash->len                 = 0U;
  return k_ra8_ok;
}

/** @brief Record bytes fed to the modelled SHA-256 implementation. @details
 * Implements the mdl sha update fixture operation used only by this focused
 * test executable. @param[in,out] ctx Fixture argument governed by the
 * exercised interface contract. @param[in] data Fixture argument governed by
 * the exercised interface contract. @param[in] len Fixture argument governed by
 * the exercised interface contract. @return RA8 status from the exercised
 * fixture operation. @retval k_ra8_ok The fixture operation completed
 * successfully. @pre Fixed-capacity fixture storage required by this operation
 * is available. @pre Arguments follow the interface contract exercised by this
 * helper. @post Documented outputs contain the exercised result when the
 * operation succeeds. @post Mutations remain confined to documented outputs and
 * file-local fixture state. @note File-local helper; no ownership escapes this
 * focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_sha_update(void* ctx, const uint8_t* data, uint16_t len)
{
  internal_mdl_hash_t* hash = (internal_mdl_hash_t*)ctx;
  if ((uint32_t)hash->len + len > sizeof(hash->bytes)) {
    return k_ra8_err_no_mem;
  }
  memcpy(&hash->bytes[hash->len], data, len);
  hash->len += len;
  return k_ra8_ok;
}

/** @brief Emit the model digest, optionally corrupted. @details Implements the
 * mdl sha final fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[in] out Fixture argument governed by the exercised interface
 * contract. @return RA8 status from the exercised fixture operation. @retval
 * k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_sha_final(void* ctx, uint8_t out[k_ra8_mdl_sha256_bytes])
{
  const internal_mdl_hash_t* hash = (const internal_mdl_hash_t*)ctx;
  memset(out,
         hash->bad_digest ? k_internal_mdl_bad_digest_fill : k_internal_mdl_digest_fill,
         k_ra8_mdl_sha256_bytes);
  return k_ra8_ok;
}

/** @brief Expose cooperative cancellation from the fixture. @details Implements
 * the mdl cancelled fixture operation used only by this focused test
 * executable. @param[in,out] ctx Fixture argument governed by the exercised
 * interface contract. @return Whether the named fixture condition holds.
 * @retval true The named fixture condition holds. @pre Fixed-capacity fixture
 * storage required by this operation is available. @pre Arguments follow the
 * interface contract exercised by this helper. @post Documented outputs contain
 * the exercised result when the operation succeeds. @post Mutations remain
 * confined to documented outputs and file-local fixture state. @note File-local
 * helper; no ownership escapes this focused test executable. @since Version
 * 0.1.0 */
RA8_INTERNAL
static bool internal_mdl_cancelled(void* ctx)
{
  return *(const bool*)ctx;
}

/** @brief Reset the transfer fixture and fill a complete bounded configuration.
 * @details Implements the mdl transfer cfg fixture operation used only by this
 * focused test executable. @param[in,out] config Fixture argument governed by
 * the exercised interface contract. @pre Fixed-capacity fixture storage
 * required by this operation is available. @pre Arguments follow the interface
 * contract exercised by this helper. @post Documented outputs contain the
 * exercised result when the operation succeeds. @post Mutations remain confined
 * to documented outputs and file-local fixture state. @note File-local helper;
 * no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_mdl_transfer_cfg(ra8_mdl_transfer_config_t* config)
{
  s_mdl_transfer = (internal_mdl_transfer_fixture_t){};
  *config        = (ra8_mdl_transfer_config_t){
    .storage          = {.begin    = internal_mdl_store_begin,
                         .write    = internal_mdl_store_write,
                         .validate = internal_mdl_store_validate,
                         .commit   = internal_mdl_store_commit,
                         .abort    = internal_mdl_store_abort,
                         .ctx      = &s_mdl_transfer.store},
    .sha256           = {.init   = internal_mdl_sha_init,
                         .update = internal_mdl_sha_update,
                         .final  = internal_mdl_sha_final,
                         .ctx    = &s_mdl_transfer.hash},
    .cancel_requested = internal_mdl_cancelled,
    .cancel_ctx       = &s_mdl_transfer.cancel,
    .chunk_bytes      = 4U,
    .max_chunks       = 8U,
  };
}

/** @brief Run one coordinator transfer with the supplied configuration.
 * @details Implements the mdl transfer run fixture operation used only by this
 * focused test executable. @param[in] config Fixture argument governed by the
 * exercised interface contract. @return RA8 status from the exercised fixture
 * operation. @retval k_ra8_ok The fixture operation completed successfully.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_mdl_transfer_run(const ra8_mdl_transfer_config_t* config)
{
  ra8_mdl_transfer_result_t result = {};
  return ra8_c6link_mdl_transfer(priv_c6link_test_link(),
                                 "https://example.test/book",
                                 "/library/book.rabook",
                                 config,
                                 &result);
}

/**
 * @par MC/DC:
 * Exercises the false branch of every failure/cancellation decision and the
 * COMPLETE branch after three ordered pulls. Other tests below vary each
 * injected condition independently. @brief Verify media transfer commits
 * verified bytes behavior. @details Executes the media transfer commits
 * verified bytes scenario with bounded fixture state and asserts the
 * contract-specific result. @pre Fixed-capacity fixture storage required by
 * this operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_media_transfer_commits_verified_bytes(void)
{
  TEST_BEGIN("c6link media transfer verified commit");
  priv_c6link_test_bringup();
  ra8_mdl_transfer_config_t config = {};
  internal_mdl_transfer_cfg(&config);
  TEST_ASSERT_EQ(k_ra8_ok, internal_mdl_transfer_run(&config));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.begins);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.commits);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.validations);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_mdl_transfer.store.aborts);
  TEST_ASSERT_EQ((int64_t)6, (int64_t)s_mdl_transfer.store.len);
  TEST_ASSERT(memcmp(s_mdl_transfer.store.bytes, "abcdef", 6U) == 0);
  TEST_ASSERT(memcmp(s_mdl_transfer.hash.bytes, "abcdef", 6U) == 0);
  TEST_END("c6link media transfer verified commit");
}

/**
 * @par MC/DC:
 * V1 full write succeeds; V2 short-write=true alone changes the decision to
 * invalid-size; V3 write_error=no_mem alone selects media removal/full error.
 * @brief Verify media transfer aborts storage failures behavior. @details
 * Executes the media transfer aborts storage failures scenario with bounded
 * fixture state and asserts the contract-specific result. @pre Fixed-capacity
 * fixture storage required by this operation is available. @pre Arguments
 * follow the interface contract exercised by this helper. @post Documented
 * outputs contain the exercised result when the operation succeeds. @post
 * Mutations remain confined to documented outputs and file-local fixture state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_media_transfer_aborts_storage_failures(void)
{
  TEST_BEGIN("c6link media transfer storage failures");
  ra8_mdl_transfer_config_t config = {};
  priv_c6link_test_bringup();
  internal_mdl_transfer_cfg(&config);
  s_mdl_transfer.store.short_write = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_mdl_transfer_run(&config));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.aborts);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_mdl_transfer.store.commits);

  priv_c6link_test_bringup();
  internal_mdl_transfer_cfg(&config);
  s_mdl_transfer.store.write_error = k_ra8_err_no_mem;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, internal_mdl_transfer_run(&config));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.aborts);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_mdl_transfer.store.commits);
  TEST_END("c6link media transfer storage failures");
}

/**
 * @brief Reject digest mismatch and out-of-order model responses
 * transactionally.
 * @par MC/DC:
 * For the response-valid conjunction in `mdl_take_chunk`, V1 is a normally
 * ordered response (all correlation/size/semantic conditions true) in the
 * bad-digest and storage-validation runs. V2 is the same first response with
 * sequence incremented by one, making only
 * `msg->sequence == session->next_sequence` false and the whole conjunction
 * false; V1/V2 prove that condition independently decides. After valid
 * transfer, `(!chunk->has_sha256) || (chunk->total_bytes != bytes_stored)`
 * executes F,F/false before the digest-only mismatch and validation-only
 * failure select their respective single decisions.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl.c@mdl_take_chunk
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c@mdl_transfer_commit
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c@ra8_c6link_mdl_transfer
 * @details Executes the media transfer aborts integrity failures scenario with
 * bounded fixture state and asserts the contract-specific result. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_media_transfer_aborts_integrity_failures(void)
{
  TEST_BEGIN("c6link media transfer integrity failures");
  ra8_mdl_transfer_config_t config = {};
  priv_c6link_test_bringup();
  internal_mdl_transfer_cfg(&config);
  s_mdl_transfer.hash.bad_digest = true;
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch, internal_mdl_transfer_run(&config));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.aborts);

  priv_c6link_test_bringup();
  internal_mdl_transfer_cfg(&config);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_out_of_order;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, internal_mdl_transfer_run(&config));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.aborts);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)ra8_c6_model()->mdl_cancels);

  priv_c6link_test_bringup();
  internal_mdl_transfer_cfg(&config);
  s_mdl_transfer.store.validation_error = k_ra8_err_validation_failed;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, internal_mdl_transfer_run(&config));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.validations);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_mdl_transfer.store.commits);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.aborts);
  TEST_END("c6link media transfer integrity failures");
}

/**
 * @brief Cancel before the first pull and abort all local temporary state.
 * @par MC/DC:
 * The cancellation decision
 * `(cancel_requested != nullptr) && cancel_requested(cancel_ctx)` executes
 * V1 T,T/true here, before the first pull. Its companion control in
 * `internal_test_media_transfer_commits_verified_bytes` is V2 T,F/false with
 * the same non-null callback; V1/V2 independently vary the callback result.
 * This vector also leaves the remote session active for the abort path, proving
 * remote cancel and local temporary-storage abort both occur before return.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c@ra8_c6link_mdl_transfer
 * Decisions: libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c@mdl_transfer_abort
 * @details Executes the media transfer cancellation is atomic scenario with
 * bounded fixture state and asserts the contract-specific result. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_media_transfer_cancellation_is_atomic(void)
{
  TEST_BEGIN("c6link media transfer cancellation");
  priv_c6link_test_bringup();
  ra8_mdl_transfer_config_t config = {};
  internal_mdl_transfer_cfg(&config);
  s_mdl_transfer.cancel = true;
  TEST_ASSERT_EQ(k_ra8_err_cancelled, internal_mdl_transfer_run(&config));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_mdl_transfer.store.aborts);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_mdl_transfer.store.commits);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)ra8_c6_model()->mdl_cancels);
  TEST_END("c6link media transfer cancellation");
}

int32_t main(void)
{
  internal_test_media_download_roundtrip();
  internal_test_media_download_rejects_bad_terminal_frames();
  internal_test_media_download_terminal_status();
  internal_test_media_download_rejects_unknown_response_fields();
  internal_test_media_transfer_commits_verified_bytes();
  internal_test_media_transfer_aborts_storage_failures();
  internal_test_media_transfer_aborts_integrity_failures();
  internal_test_media_transfer_cancellation_is_atomic();
  return 0;
}
