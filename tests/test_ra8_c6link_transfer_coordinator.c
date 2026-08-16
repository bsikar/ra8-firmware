/**
 * @file test_ra8_c6link_transfer_coordinator.c
 * @brief Isolated transactional-coordinator sequencing and MC/DC vectors
 * @details Drives the bounded pull budget, the remote-cancellation dispatch
 * arm, and the terminal-metadata guard of the transfer coordinator from one
 * self-contained fixture, so the already-large media suite keeps only its
 * happy-path, storage-fault, and integrity-fault vectors.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_c6_model.h"
#include "ra8_c6link_mdl_transfer_internal.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_err.h"
#include "test_ra8_c6link_transfer_coordinator_internal.h"
#include "unity_minimal.h"

/** @enum internal_coordinator_limit_t @brief Bounded coordinator fixture geometry. */
typedef enum : uint16_t {
  k_internal_store_cap        = 16U,   /**< Temporary-object byte capacity.        */
  k_internal_chunk_bytes      = 4U,    /**< Bytes requested per remote pull.       */
  k_internal_budget_one       = 1U,    /**< One-pull budget, shorter than the job. */
  k_internal_budget_full      = 8U,    /**< Budget that outlasts the model source. */
  k_internal_source_bytes     = 6U,    /**< Bytes the model serves in total.       */
  k_internal_terminal_seq     = 3U,    /**< Responses a full model job consumes.   */
  k_internal_terminal_job     = 1U,    /**< Synthetic terminal-chunk job identity. */
  k_internal_digest_fill      = 0xA5U, /**< Digest octet both fixture sides agree. */
  k_internal_advertised_extra = 1U,    /**< Overstatement of the durable length.   */
} internal_coordinator_limit_t;

/** @brief Bounded observations for one modelled storage transaction. */
typedef struct {
  uint8_t  bytes[k_internal_store_cap]; /**< Bytes accepted by temporary storage. */
  uint16_t len;                         /**< Valid stored byte count.             */
  uint8_t  begins;                      /**< Begin callback count.                */
  uint8_t  validations;                 /**< Validate callback count.             */
  uint8_t  commits;                     /**< Commit callback count.               */
  uint8_t  aborts;                      /**< Abort callback count.                */
} internal_store_t;

/** @brief Streaming-hash fixture emitting one fixed agreed digest. */
typedef struct {
  uint16_t len; /**< Bytes observed by the modelled hash stream. */
} internal_hash_t;

/** @brief One complete set of coordinator seams and their observations. */
typedef struct {
  internal_store_t store; /**< Transactional storage fixture. */
  internal_hash_t  hash;  /**< Streaming hash fixture.        */
} internal_fixture_t;

static internal_fixture_t s_fixture;
static ra8_mdl_chunk_t    s_terminal;

/** @brief Destination every scenario in this translation unit publishes to. */
static const char s_destination[] = "/library/coordinator.rabook";

/** @brief Begin one modelled temporary object. @details Implements the store
 * begin fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[in] destination Fixture argument governed by the exercised
 * interface contract. @return RA8 status from the exercised fixture operation.
 * @retval k_ra8_ok The fixture operation completed successfully. @retval
 * k_ra8_err_invalid_arg The coordinator supplied an unexpected destination.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_store_begin(void* ctx, const char* destination)
{
  internal_store_t* store = (internal_store_t*)ctx;
  if ((store == nullptr) || (strcmp(destination, s_destination) != 0)) {
    return k_ra8_err_invalid_arg;
  }
  store->begins += 1U;
  store->len = 0U;
  return k_ra8_ok;
}

/** @brief Append one exact pull to modelled temporary storage. @details
 * Implements the store write fixture operation used only by this focused test
 * executable. @param[in,out] ctx Fixture argument governed by the exercised
 * interface contract. @param[in] data Fixture argument governed by the
 * exercised interface contract. @param[in] len Fixture argument governed by the
 * exercised interface contract. @param[out] written Fixture argument governed
 * by the exercised interface contract. @return RA8 status from the exercised
 * fixture operation. @retval k_ra8_ok The fixture operation completed
 * successfully. @retval k_ra8_err_no_mem The pull exceeded fixture capacity.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t
internal_store_write(void* ctx, const uint8_t* data, uint16_t len, uint16_t* written)
{
  internal_store_t* store = (internal_store_t*)ctx;
  *written                = 0U;
  if (((uint32_t)store->len + len) > sizeof(store->bytes)) {
    return k_ra8_err_no_mem;
  }
  (void)memcpy(&store->bytes[store->len], data, len);
  store->len += len;
  *written = len;
  return k_ra8_ok;
}

/** @brief Accept the private object before publication. @details Implements the
 * store validate fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[in] total_bytes Fixture argument governed by the exercised
 * interface contract. @param[in] sha256 Fixture argument governed by the
 * exercised interface contract. @return RA8 status from the exercised fixture
 * operation. @retval k_ra8_ok The fixture operation completed successfully.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_store_validate(void*         ctx,
                                         uint64_t      total_bytes,
                                         const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  internal_store_t* store = (internal_store_t*)ctx;
  (void)total_bytes;
  (void)sha256;
  store->validations += 1U;
  return k_ra8_ok;
}

/** @brief Publish the modelled object. @details Implements the store commit
 * fixture operation used only by this focused test executable.
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
static ra8_err_t internal_store_commit(void* ctx)
{
  internal_store_t* store = (internal_store_t*)ctx;
  store->commits += 1U;
  return k_ra8_ok;
}

/** @brief Destroy the modelled temporary object. @details Implements the store
 * abort fixture operation used only by this focused test executable.
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
static ra8_err_t internal_store_abort(void* ctx)
{
  internal_store_t* store = (internal_store_t*)ctx;
  store->aborts += 1U;
  store->len = 0U;
  return k_ra8_ok;
}

/** @brief Reset the modelled hash stream. @details Implements the sha init
 * fixture operation used only by this focused test executable.
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
static ra8_err_t internal_sha_init(void* ctx)
{
  internal_hash_t* hash = (internal_hash_t*)ctx;
  hash->len             = 0U;
  return k_ra8_ok;
}

/** @brief Count bytes fed to the modelled hash stream. @details Implements the
 * sha update fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[in] data Fixture argument governed by the exercised
 * interface contract. @param[in] len Fixture argument governed by the exercised
 * interface contract. @return RA8 status from the exercised fixture operation.
 * @retval k_ra8_ok The fixture operation completed successfully. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_sha_update(void* ctx, const uint8_t* data, uint16_t len)
{
  internal_hash_t* hash = (internal_hash_t*)ctx;
  (void)data;
  hash->len += len;
  return k_ra8_ok;
}

/** @brief Emit the one digest both fixture sides agree on. @details Implements
 * the sha final fixture operation used only by this focused test executable.
 * @param[in,out] ctx Fixture argument governed by the exercised interface
 * contract. @param[out] out Fixture argument governed by the exercised
 * interface contract. @return RA8 status from the exercised fixture operation.
 * @retval k_ra8_ok The fixture operation completed successfully. @pre
 * Fixed-capacity fixture storage required by this operation is available. @pre
 * Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_sha_final(void* ctx, uint8_t out[k_ra8_mdl_sha256_bytes])
{
  (void)ctx;
  (void)memset(out, (int)k_internal_digest_fill, (size_t)k_ra8_mdl_sha256_bytes);
  return k_ra8_ok;
}

/** @brief Reset the fixture and fill one complete bounded configuration.
 * @details Implements the configuration fixture operation used only by this
 * focused test executable. @param[out] config Fixture argument governed by the
 * exercised interface contract. @pre Fixed-capacity fixture storage required by
 * this operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_cfg(ra8_mdl_transfer_config_t* config)
{
  s_fixture = (internal_fixture_t){};
  *config   = (ra8_mdl_transfer_config_t){
    .storage     = {.begin    = internal_store_begin,
                    .write    = internal_store_write,
                    .validate = internal_store_validate,
                    .commit   = internal_store_commit,
                    .abort    = internal_store_abort,
                    .ctx      = &s_fixture.store},
    .sha256      = {.init   = internal_sha_init,
                    .update = internal_sha_update,
                    .final  = internal_sha_final,
                    .ctx    = &s_fixture.hash},
    .format      = k_ra8_mdl_format_rabook,
    .chunk_bytes = k_internal_chunk_bytes,
    .max_chunks  = k_internal_budget_full,
  };
}

/** @brief Run one coordinator transfer against the shared model link. @details
 * Implements the transfer fixture operation used only by this focused test
 * executable. @param[in] config Fixture argument governed by the exercised
 * interface contract. @param[out] result Fixture argument governed by the
 * exercised interface contract. @return RA8 status from the exercised fixture
 * operation. @retval k_ra8_ok The fixture operation completed successfully.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper. @post
 * Documented outputs contain the exercised result when the operation succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state. @note File-local helper; no ownership escapes this focused test
 * executable. @since Version 0.1.0 */
RA8_INTERNAL
static ra8_err_t internal_run(const ra8_mdl_transfer_config_t* config,
                              ra8_mdl_transfer_result_t*       result)
{
  return ra8_c6link_mdl_transfer(priv_c6link_test_link(),
                                 "https://example.test/book",
                                 s_destination,
                                 config,
                                 result);
}

/**
 * @test priv_test_c6link_transfer_coordinator_run
 * @brief Time out and unwind when the pull budget ends before the job does.
 * @par MC/DC:
 * Decision: `(pull < config->max_chunks) && (err == k_ra8_ok)` (2 conditions)
 * - Vector 1: pull=0 < 1, err=ok -> T,T -> true (the one pull this budget
 *   allows, supplied here).
 * - Vector 2: pull=1, max_chunks=1, err=ok -> F,- -> false (supplied here;
 *   the budget, not a fault, ends the loop).
 * - Vector 3: pull=1 < 8, err=invalid_size -> T,F -> false (supplied by
 *   `internal_test_media_transfer_aborts_storage_failures`).
 * Vectors 1+2 prove the budget operand independently decides; 1+3 prove the
 * same for the running status. N+1 = 3 vectors for N=2.
 * Decision: `(err == k_ra8_ok) && state.session.active` (2 conditions)
 * - Vector 1: err=ok, session active -> T,T -> true (supplied here; the only
 *   way the loop can end with no cause is exhausting the budget).
 * - Vector 2: err=invalid_size -> F,- -> false (supplied by the same storage
 *   failure vector above).
 * The T,F vector is unreachable and the decision carries the matching
 * `mcdc-deactivated` rationale in the source: err stays `k_ra8_ok` only on
 * budget exhaustion, and every response that deactivates the session either
 * returns from inside the loop or sets a cause.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c@ra8_c6link_mdl_transfer
 * @details Serves a six-byte object through the model in four-byte pulls under
 * a one-pull budget, so the coordinator must report a timeout, cancel the still
 * active remote job, and destroy the temporary object without publishing it.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @pre The model source is longer than one pull of `chunk_bytes`.
 * @post The transfer reports `k_ra8_err_timeout` and commits nothing.
 * @post The still-active remote job is cancelled exactly once.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_budget_exhaustion_times_out(void)
{
  TEST_BEGIN("c6link transfer pull-budget exhaustion");
  priv_c6link_test_bringup();
  ra8_mdl_transfer_config_t config = {};
  ra8_mdl_transfer_result_t result = {};
  internal_cfg(&config);
  config.max_chunks = k_internal_budget_one;
  TEST_ASSERT_EQ(k_ra8_err_timeout, internal_run(&config, &result));
  TEST_ASSERT_EQ(1, s_fixture.store.begins);
  TEST_ASSERT_EQ(k_internal_chunk_bytes, s_fixture.hash.len);
  TEST_ASSERT_EQ(0, s_fixture.store.validations);
  TEST_ASSERT_EQ(0, s_fixture.store.commits);
  TEST_ASSERT_EQ(1, s_fixture.store.aborts);
  TEST_ASSERT_EQ(1, ra8_c6_model()->mdl_cancels);
  TEST_ASSERT_EQ(0, result.bytes_stored);
  TEST_END("c6link transfer pull-budget exhaustion");
}

/**
 * @test priv_test_c6link_transfer_coordinator_run
 * @brief Unwind exactly once when the remote reports the job cancelled.
 * @par MC/DC:
 * Decision: `(err == k_ra8_ok) && (chunk.state == COMPLETE)` (2 conditions)
 * - Vector 1: err=ok, state=COMPLETE -> T,T -> true (supplied by
 *   `internal_test_media_transfer_commits_verified_bytes`).
 * - Vector 2: err=protocol_error -> F,- -> false (supplied by the
 *   out-of-order response vector in the same media suite).
 * - Vector 3: err=ok, state=CANCELLED -> T,F -> false (supplied here; the
 *   only remote state that reaches this arm with no cause).
 * Vectors 1+2 prove the status operand independently decides; 1+3 prove the
 * same for the terminal-state operand. N+1 = 3 vectors for N=2.
 * Decision: `(err == k_ra8_ok) && (chunk.state == CANCELLED)` (2 conditions)
 * - Vector 1: err=ok, state=CANCELLED -> T,T -> true (supplied here).
 * - Vector 2: err=protocol_error -> F,- -> false (supplied by the same
 *   out-of-order vector).
 * The T,F vector is unreachable and the decision carries the matching
 * `mcdc-deactivated` rationale in the source: DOWNLOADING and COMPLETE are
 * consumed by the preceding arms, and FAILED must carry a nonzero status.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c@ra8_c6link_mdl_transfer
 * @details Makes the model answer the first pull with a coherent CANCELLED
 * record. That response deactivates the session inside the client, so the
 * coordinator must abort local temporary state without sending a second,
 * redundant cancel to a job the remote has already ended.
 * @pre The shared C6 model fixture can be reset and brought up.
 * @pre The injected fault is consumed by the first Next response.
 * @post The transfer reports `k_ra8_err_cancelled` and commits nothing.
 * @post No cancel request is sent for an already-ended job.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_remote_cancellation_unwinds(void)
{
  TEST_BEGIN("c6link transfer remote cancellation");
  priv_c6link_test_bringup();
  ra8_mdl_transfer_config_t config = {};
  ra8_mdl_transfer_result_t result = {};
  internal_cfg(&config);
  ra8_c6_model()->mdl_fault = k_c6m_mdl_fault_cancelled;
  TEST_ASSERT_EQ(k_ra8_err_cancelled, internal_run(&config, &result));
  TEST_ASSERT_EQ(1, s_fixture.store.begins);
  TEST_ASSERT_EQ(0, s_fixture.store.len);
  TEST_ASSERT_EQ(0, s_fixture.store.validations);
  TEST_ASSERT_EQ(0, s_fixture.store.commits);
  TEST_ASSERT_EQ(1, s_fixture.store.aborts);
  TEST_ASSERT_EQ(0, ra8_c6_model()->mdl_cancels);
  TEST_END("c6link transfer remote cancellation");
}

/** @brief Build the agreed terminal chunk the metadata vectors start from.
 * @details Implements the terminal chunk fixture operation used only by this
 * focused test executable. @pre Fixed-capacity fixture storage required by this
 * operation is available. @pre Arguments follow the interface contract
 * exercised by this helper. @post Documented outputs contain the exercised
 * result when the operation succeeds. @post Mutations remain confined to
 * documented outputs and file-local fixture state. @note File-local helper; no
 * ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_terminal_chunk(void)
{
  s_terminal = (ra8_mdl_chunk_t){
    .job_id      = k_internal_terminal_job,
    .sequence    = k_internal_terminal_seq,
    .offset      = k_internal_source_bytes,
    .total_bytes = k_internal_source_bytes,
    .state       = k_ra8_mdl_state_complete,
    .has_sha256  = true,
  };
  (void)memset(s_terminal.sha256, (int)k_internal_digest_fill, sizeof(s_terminal.sha256));
}

/**
 * @test priv_test_c6link_transfer_coordinator_run
 * @brief Refuse to publish unless the terminal record describes what was
 * stored.
 * @par MC/DC:
 * Decision: `(!chunk->has_sha256) || (chunk->total_bytes != bytes_stored)`
 * (2 conditions)
 * - Vector 1: digest present, total 6 == stored 6 -> F,F -> false (control:
 *   the stage finalises, compares, validates, and publishes).
 * - Vector 2: digest absent, total 6 == stored 6 -> T,- -> true (varies the
 *   digest-presence operand only).
 * - Vector 3: digest present, total 7 != stored 6 -> F,T -> true (varies the
 *   advertised-length operand only).
 * Vectors 1+2 prove digest presence independently decides; 1+3 prove the same
 * for length agreement. N+1 = 3 vectors for N=2.
 * Decisions:
 * libs/ra8_c6link/src/ra8_c6link_mdl_transfer.c@internal_mdl_transfer_commit
 * @details Reaches the stage through its private test seam because the
 * chunk-semantics validator in `ra8_c6link_mdl.c` requires a 32-byte digest on
 * every COMPLETE response, so no public-API transfer can present vector 2.
 * @pre A complete configuration with storage and hash seams is available.
 * @pre The fixture digest and the terminal chunk digest agree by construction.
 * @post Only the control vector publishes the object.
 * @post Both rejected vectors leave the transaction uncommitted.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_terminal_metadata_guard(void)
{
  TEST_BEGIN("c6link transfer terminal metadata guard");
  ra8_mdl_transfer_config_t config = {};
  ra8_mdl_transfer_result_t result = {};

  internal_cfg(&config);
  internal_terminal_chunk();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_mdl_transfer_commit_test(&config,
                                                     &s_terminal,
                                                     k_internal_source_bytes,
                                                     k_internal_terminal_seq,
                                                     &result));
  TEST_ASSERT_EQ(1, s_fixture.store.validations);
  TEST_ASSERT_EQ(1, s_fixture.store.commits);
  TEST_ASSERT_EQ(k_internal_source_bytes, result.bytes_stored);
  TEST_ASSERT_EQ(k_internal_terminal_seq, result.chunks_received);
  TEST_ASSERT(memcmp(result.sha256, s_terminal.sha256, sizeof(result.sha256)) == 0);

  internal_cfg(&config);
  internal_terminal_chunk();
  s_terminal.has_sha256 = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_c6link_mdl_transfer_commit_test(&config,
                                                     &s_terminal,
                                                     k_internal_source_bytes,
                                                     k_internal_terminal_seq,
                                                     &result));
  TEST_ASSERT_EQ(0, s_fixture.store.validations);
  TEST_ASSERT_EQ(0, s_fixture.store.commits);

  internal_cfg(&config);
  internal_terminal_chunk();
  s_terminal.total_bytes = (uint64_t)k_internal_source_bytes + k_internal_advertised_extra;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_c6link_mdl_transfer_commit_test(&config,
                                                     &s_terminal,
                                                     k_internal_source_bytes,
                                                     k_internal_terminal_seq,
                                                     &result));
  TEST_ASSERT_EQ(0, s_fixture.store.validations);
  TEST_ASSERT_EQ(0, s_fixture.store.commits);
  TEST_END("c6link transfer terminal metadata guard");
}

RA8_PRIV void priv_test_c6link_transfer_coordinator_run(void)
{
  internal_test_budget_exhaustion_times_out();
  internal_test_remote_cancellation_unwinds();
  internal_test_terminal_metadata_guard();
}
