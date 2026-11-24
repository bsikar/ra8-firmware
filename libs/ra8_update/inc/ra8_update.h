/**
 * @file ra8_update.h
 * @brief Unified stage/verify/commit/rollback pipeline seam for OTA and DFU
 *
 * @par Tag
 * [Ring 4 / Service] {World: Any}
 *
 * @details
 * Implements the shared pipeline state machine declared in
 * `docs/design/unified_update_pipeline.md`. Both OTA (`libs/ra8_ota`) and DFU
 * (`libs/ra8_dfu`) express staging, verification, activation, confirmation, and
 * rollback transitions through one caller-owned ::ra8_update_session_t backed by
 * injected storage, crypto, anti-rollback, and commit vtables.
 *
 * Every struct uses explicit padding fields so 32-bit Cortex-M85 and 64-bit
 * host targets agree identically on field offsets and sizes. No dynamic memory
 * allocation is performed anywhere in this module (NASA Rule 3).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_update_limits_t
 * @brief Compile-time sizes, alignment bounds, and default trial budget.
 */
typedef enum : uint32_t {
  k_ra8_update_sha256_bytes  = 32U,      /**< SHA-256 digest length in bytes.        */
  k_ra8_update_sig_max_bytes = 96U,      /**< Signature max bytes (raw 64 or ASN.1). */
  k_ra8_update_page_bytes    = 32U,      /**< RA8D2 MRAM page alignment in bytes.    */
  k_ra8_update_chunk_bytes   = 4096U,    /**< Recommended verify rehash chunk cap.   */
  k_ra8_update_max_bytes     = 0x80000U, /**< Upper payload bound (512 KiB).         */
  k_ra8_update_max_meta      = 512U,     /**< Maximum bound metadata preimage bytes. */
  k_ra8_update_default_tries = 3U,       /**< Default DFU trial attempt budget.      */
} ra8_update_limits_t;

/**
 * @enum ra8_update_transport_t
 * @brief Ingress transport selector declared by a pipeline.
 */
typedef enum : uint8_t {
  k_ra8_update_transport_ota = 0U, /**< Pull stream over HTTPS / link. */
  k_ra8_update_transport_dfu = 1U, /**< Push ingress over USB DFU.     */
  k_ra8_update_transport_mem = 2U, /**< Pre-staged memory/test input.  */
} ra8_update_transport_t;

/**
 * @enum ra8_update_payload_t
 * @brief Payload packaging mode accepted by a declared pipeline.
 */
typedef enum : uint8_t {
  k_ra8_update_payload_bundle       = 0U, /**< Signed N-image RA8U container.  */
  k_ra8_update_payload_manifest_raw = 1U, /**< OTA JSON manifest + raw binary. */
  k_ra8_update_payload_rot1_slot    = 2U, /**< DFU body + trailing ROT1 block. */
} ra8_update_payload_t;

/**
 * @enum ra8_update_apply_t
 * @brief Activation / commit strategy for staged updates.
 */
typedef enum : uint8_t {
  k_ra8_update_apply_ab_slots = 0U, /**< Copy-to-run A/B MRAM slots + boot_state. */
  k_ra8_update_apply_ab_banks = 1U, /**< Hardware/software A/B bank startup swap. */
  k_ra8_update_apply_staged   = 2U, /**< Common staging store before chip apply.  */
} ra8_update_apply_t;

/**
 * @enum ra8_update_stage_t
 * @brief Explicit lifecycle stage of ::ra8_update_session_t.
 */
typedef enum : uint8_t {
  k_ra8_update_stage_idle          = 0U,  /**< Initialized, ready for begin.     */
  k_ra8_update_stage_preflight     = 1U,  /**< Storage prepare in flight.        */
  k_ra8_update_stage_staging       = 2U,  /**< Accepting sequential chunks.      */
  k_ra8_update_stage_staged        = 3U,  /**< Full payload written to target.   */
  k_ra8_update_stage_verifying     = 4U,  /**< Rehash + signature check running. */
  k_ra8_update_stage_verified      = 5U,  /**< Authenticated; safe to commit.    */
  k_ra8_update_stage_committing    = 6U,  /**< Activation hook executing.        */
  k_ra8_update_stage_trial_pending = 7U,  /**< Trial commit awaiting confirm.    */
  k_ra8_update_stage_confirmed     = 8U,  /**< Confirmed and floor advanced.     */
  k_ra8_update_stage_rolled_back   = 9U,  /**< Aborted or rolled back safely.    */
  k_ra8_update_stage_failed        = 10U, /**< Terminal error without rollback.  */
} ra8_update_stage_t;

/**
 * @struct ra8_update_pipeline_decl_t
 * @brief Fixed 8-byte declarative update pipeline policy.
 */
typedef struct {
  ra8_update_transport_t transport;        /**< Selected ingress transport.           */
  ra8_update_payload_t   payload;          /**< Expected payload container format.    */
  ra8_update_apply_t     apply_mode;       /**< Selected activation/commit strategy.  */
  uint8_t                trial_budget;     /**< 0 = immediate; >0 = trial boots.      */
  bool                   enforce_rot;      /**< Require ECDSA verification gate.      */
  bool                   enforce_rollback; /**< Require monotonic floor enforcement.  */
  bool                   defer_floor_bump; /**< True = advance floor at confirm only. */
  uint8_t                reserved0;        /**< Must be zero.                         */
} ra8_update_pipeline_decl_t;

static_assert(sizeof(ra8_update_pipeline_decl_t) == 8U,
              "ra8_update_pipeline_decl_t must remain 8 bytes across targets");

/**
 * @struct ra8_update_storage_iface_t
 * @brief Injected inactive-slot / staging storage seam (DIP).
 */
typedef struct {
  /** @brief Prepare/erase target storage for @p total_hint_bytes. */
  ra8_err_t (*prepare)(void* ctx, uint32_t total_hint_bytes);
  /** @brief Program @p len bytes from @p data at byte @p offset. */
  ra8_err_t (*write)(void* ctx, uint32_t offset, const uint8_t* data, uint32_t len);
  /** @brief Read back @p len bytes at @p offset into @p out for rehash. */
  ra8_err_t (*readback)(void* ctx, uint32_t offset, uint8_t* out, uint32_t len);
  /** @brief Caller-owned context pointer. */
  void* ctx;
} ra8_update_storage_iface_t;

/**
 * @struct ra8_update_crypto_iface_t
 * @brief Injected SHA-256 + ECDSA verification seam (DIP).
 */
typedef struct {
  /** @brief Initialize a streaming SHA-256 context. */
  ra8_err_t (*sha256_init)(void* ctx);
  /** @brief Feed @p len bytes into the SHA-256 digest. */
  ra8_err_t (*sha256_update)(void* ctx, const uint8_t* data, uint32_t len);
  /** @brief Finalize the 32-byte digest into @p out. */
  ra8_err_t (*sha256_final)(void* ctx, uint8_t out[k_ra8_update_sha256_bytes]);
  /** @brief Verify @p sig of @p sig_len bytes against @p digest using @p pubkey_handle. */
  ra8_err_t (*ecdsa_verify)(void*          ctx,
                            uint32_t       pubkey_handle,
                            const uint8_t  digest[k_ra8_update_sha256_bytes],
                            const uint8_t* sig,
                            uint32_t       sig_len);
  /** @brief Caller-owned context pointer. */
  void* ctx;
} ra8_update_crypto_iface_t;

/**
 * @struct ra8_update_rollback_iface_t
 * @brief Injected monotonic version-floor seam (DIP).
 */
typedef struct {
  /** @brief Read the current persisted minimum accepted image version. */
  ra8_err_t (*read_floor)(void* ctx, uint32_t* out_min_version);
  /** @brief Durably advance the persisted version floor to @p new_version. */
  ra8_err_t (*commit_floor)(void* ctx, uint32_t new_version);
  /** @brief Caller-owned context pointer. */
  void* ctx;
} ra8_update_rollback_iface_t;

/**
 * @struct ra8_update_commit_iface_t
 * @brief Injected slot/bank activation, confirmation, and rollback hooks.
 */
typedef struct {
  /** @brief Commit verified @p total_bytes with slot/bank tag @p seq_or_bank. */
  ra8_err_t (*commit)(void*    ctx,
                      uint32_t total_bytes,
                      uint32_t seq_or_bank,
                      bool     is_trial,
                      uint8_t  trial_budget);
  /** @brief Mark an active trial image confirmed (required when trial_budget > 0). */
  ra8_err_t (*confirm)(void* ctx);
  /** @brief Optional abort/cleanup hook invoked on verify/commit failure or rollback. */
  ra8_err_t (*rollback)(void* ctx, ra8_err_t reason);
  /** @brief Caller-owned context pointer. */
  void* ctx;
} ra8_update_commit_iface_t;

/**
 * @struct ra8_update_cfg_t
 * @brief Full declarative configuration for a shared update session.
 */
typedef struct {
  ra8_update_pipeline_decl_t  decl;          /**< Transport/policy declaration.        */
  uint32_t                    pubkey_handle; /**< Trusted RoT public-key handle.       */
  uint32_t                    align_bytes;   /**< 0/1 or 32 (MRAM page alignment).     */
  uint32_t                    max_bytes;     /**< Maximum staged payload bytes.        */
  uint32_t                    reserved0;     /**< Must be zero.                        */
  ra8_update_storage_iface_t  storage;       /**< Injected staging/slot storage seam.  */
  ra8_update_crypto_iface_t   crypto;        /**< Injected SHA-256 + ECDSA seam.       */
  ra8_update_rollback_iface_t rollback;      /**< Injected anti-rollback counter seam. */
  ra8_update_commit_iface_t   commit;        /**< Injected commit/confirm/abort seam.  */
} ra8_update_cfg_t;

/**
 * @struct ra8_update_verify_req_t
 * @brief Verification inputs supplied once staging completes.
 */
typedef struct {
  uint32_t       expected_version; /**< Monotonic security version of staged image. */
  uint32_t       sig_len;          /**< Signature byte length (<= sig_max_bytes).   */
  uint32_t       meta_prefix_len;  /**< Optional metadata bytes hashed before sig.  */
  uint32_t       reserved0;        /**< Must be zero.                               */
  const uint8_t* meta_prefix;      /**< Optional metadata bytes (if len > 0).       */
  const uint8_t* expected_sha256;  /**< Optional expected digest (32 bytes) to pin. */
  const uint8_t* sig;              /**< ECDSA signature (required if enforce_rot).  */
} ra8_update_verify_req_t;

/**
 * @struct ra8_update_session_t
 * @brief Caller-owned update pipeline session context (zero heap).
 */
typedef struct {
  ra8_update_cfg_t   cfg;              /**< Copied validated config.      */
  ra8_update_stage_t stage;            /**< Current pipeline stage.       */
  uint8_t            verified;         /**< Non-zero iff verified.        */
  ra8_err_t          last_err;         /**< Most recent failure code.     */
  uint32_t           expected_bytes;   /**< Total staged bytes promised.  */
  uint32_t           bytes_written;    /**< Sequential staged byte count. */
  uint32_t           verified_version; /**< Authenticated version cached. */
  /** @brief Re-hashed SHA-256 digest captured during verification. */
  uint8_t staged_digest[k_ra8_update_sha256_bytes];
} ra8_update_session_t;

static_assert(offsetof(ra8_update_cfg_t, storage) == 24U,
              "ra8_update_cfg_t scalar header must stay 24 bytes");
static_assert(offsetof(ra8_update_verify_req_t, meta_prefix) == 16U,
              "ra8_update_verify_req_t scalar header must stay 16 bytes");

/**
 * @brief Populate the canonical declaration for @p transport
 *
 * @details
 * Produces the repository-standard ::ra8_update_pipeline_decl_t for OTA, DFU,
 * or memory-staged updates without mutating external state.
 *
 * @param[in]  transport Transport family (::ra8_update_transport_t).
 * @param[out] out_decl  Destination declaration struct (non-NULL).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Canonical declaration written to @p out_decl.
 * @retval k_ra8_err_null_ptr    @p out_decl is NULL.
 * @retval k_ra8_err_invalid_arg @p transport is outside ::ra8_update_transport_t.
 *
 * @pre @p out_decl points to writable storage.
 * @pre @p transport is a valid member of ::ra8_update_transport_t.
 * @post On ::k_ra8_ok, @p *out_decl holds the canonical policy.
 * @post On error, @p out_decl is left unmodified.
 *
 * @note Thread-safe (pure; touches no global state).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_decl_canonical(ra8_update_transport_t      transport,
                                                  ra8_update_pipeline_decl_t* out_decl);

/**
 * @brief Return whether @p stage is a resting terminal stage
 *
 * @details
 * Evaluates whether @p stage denotes completion or failure of an update attempt
 * (`confirmed`, `rolled_back`, or `failed`). Unknown values return `false`.
 *
 * @param[in] stage Pipeline stage value to classify.
 *
 * @return bool True if @p stage is terminal.
 * @retval true  @p stage is confirmed, rolled_back, or failed.
 * @retval false @p stage is active, idle, or invalid.
 *
 * @pre None.
 * @pre Callers pass an enum value from ::ra8_update_stage_t.
 * @post No state is mutated.
 * @post Return value depends solely on @p stage.
 *
 * @note Thread-safe (pure function).
 * @since 0.1.0
 */
bool ra8_update_stage_is_terminal(ra8_update_stage_t stage);

/**
 * @brief Validate configuration and initialize @p session in idle state
 *
 * @details
 * Enforces every declaration invariant (`defer_floor_bump` requires
 * `enforce_rollback`), checks all required hook pointers (`storage`, `crypto`,
 * `rollback` when enabled, `commit`, and `confirm` when `trial_budget > 0`),
 * and resets @p session cleanly into `k_ra8_update_stage_idle`.
 *
 * @param[out] session Caller-owned session storage to initialize (non-NULL).
 * @param[in]  cfg     Pipeline declaration and hook vtables (non-NULL).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Session initialized cleanly.
 * @retval k_ra8_err_null_ptr     @p session, @p cfg, or a required hook was NULL.
 * @retval k_ra8_err_invalid_arg  Declaration or alignment field invalid.
 * @retval k_ra8_err_invalid_size `max_bytes` zero or above ::k_ra8_update_max_bytes.
 *
 * @pre @p session and @p cfg are non-NULL.
 * @pre All required callbacks in @p cfg are valid for the session lifetime.
 * @post On ::k_ra8_ok, `session->stage == k_ra8_update_stage_idle`.
 * @post On error, @p session bytes are not modified.
 *
 * @note Re-entrant across distinct ::ra8_update_session_t instances.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_init(ra8_update_session_t* session, const ra8_update_cfg_t* cfg);

/**
 * @brief Open staging on the target bank or slot for @p total_bytes
 *
 * @details
 * Validates @p total_bytes against `cfg.max_bytes` and `cfg.align_bytes`, resets
 * byte counters, and invokes `storage.prepare`. Any preparation failure invokes
 * `commit.rollback` and parks @p session in `k_ra8_update_stage_rolled_back`.
 *
 * @param[in,out] session     Initialized session (non-NULL).
 * @param[in]     total_bytes Total payload bytes expected during staging.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Storage prepared and stage moved to `staging`.
 * @retval k_ra8_err_null_ptr      @p session is NULL.
 * @retval k_ra8_err_invalid_state @p session is currently active mid-transfer.
 * @retval k_ra8_err_invalid_size  @p total_bytes zero, oversized, or misaligned.
 * @retval other                   Error returned by `storage.prepare`.
 *
 * @pre @p session was initialized via ::ra8_update_init.
 * @pre @p session is in `idle`, `confirmed`, `rolled_back`, or `failed`.
 * @post On ::k_ra8_ok, @p session accepts sequential writes starting at offset 0.
 * @post On storage failure, rollback hook ran and stage is `rolled_back`.
 *
 * @note Caller-synchronized per session.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_stage_begin(ra8_update_session_t* session, uint32_t total_bytes);

/**
 * @brief Write the next sequential staged block or chunk
 *
 * @details
 * Enforces sequential write ordering (`offset == bytes_written`), non-overflow
 * (`offset + len <= expected_bytes`), and `cfg.align_bytes` boundaries (all
 * non-final chunks aligned), then delegates to `storage.write`. A write failure
 * invokes rollback and moves @p session to `k_ra8_update_stage_rolled_back`.
 *
 * @param[in,out] session Active staging session (non-NULL).
 * @param[in]     offset  Byte offset matching `session->bytes_written`.
 * @param[in]     data    Source buffer of @p len bytes (non-NULL).
 * @param[in]     len     Non-zero chunk length in bytes.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Chunk written and `bytes_written` advanced.
 * @retval k_ra8_err_null_ptr      @p session or @p data is NULL.
 * @retval k_ra8_err_invalid_state @p session not in `k_ra8_update_stage_staging`.
 * @retval k_ra8_err_invalid_arg   @p offset discontinuous or misaligned.
 * @retval k_ra8_err_invalid_size  @p len zero, misaligned, or overflows target.
 * @retval other                   Error returned by `storage.write`.
 *
 * @pre ::ra8_update_stage_begin returned ::k_ra8_ok.
 * @pre @p offset equals `session->bytes_written`.
 * @post `session->bytes_written` increases by @p len on success.
 * @post Active boot firmware stays untouched even on write failure.
 *
 * @note Caller-synchronized per session.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_stage_write(ra8_update_session_t* session,
                                               uint32_t              offset,
                                               const uint8_t*        data,
                                               uint32_t              len);

/**
 * @brief Finalize staging once all promised bytes have arrived
 *
 * @details
 * Verifies `bytes_written == expected_bytes` and advances @p session to
 * `k_ra8_update_stage_staged`. Truncated staging rolls back immediately.
 *
 * @param[in,out] session Staging session (non-NULL).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Full payload staged (`stage == staged`).
 * @retval k_ra8_err_null_ptr      @p session is NULL.
 * @retval k_ra8_err_invalid_state @p session not in `staging`.
 * @retval k_ra8_err_invalid_size  Staged byte count short of `expected_bytes`.
 *
 * @pre @p session is in `k_ra8_update_stage_staging`.
 * @pre Caller has delivered all @p expected_bytes via ::ra8_update_stage_write.
 * @post On ::k_ra8_ok, ::ra8_update_verify may proceed.
 * @post On truncation, rollback hook runs and stage enters `rolled_back`.
 *
 * @note Caller-synchronized per session.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_stage_finish(ra8_update_session_t* session);

/**
 * @brief Re-hash staged storage, verify RoT signature, and check version floor
 *
 * @details
 * Reads staged bytes back in bounded chunks using @p scratch (`scratch_cap`
 * bytes), compares the SHA-256 against `req->expected_sha256` (constant-time)
 * when non-NULL, binds optional metadata ahead of the digest and checks ECDSA
 * when `decl.enforce_rot` is true, and verifies `req->expected_version >=
 * stored_floor` when `decl.enforce_rollback` is true.
 *
 * Any failure triggers `commit.rollback` and leaves `verified == 0` so commit
 * can never run on an unverified image.
 *
 * @param[in,out] session     Staged session (non-NULL).
 * @param[in]     req         Verification parameters and optional metadata/sig.
 * @param[in,out] scratch     Caller-provided chunk buffer for storage readback.
 * @param[in]     scratch_cap Size of @p scratch in bytes (must be > 0).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                    Authenticated and rollback policy passed.
 * @retval k_ra8_err_null_ptr          Required pointer NULL.
 * @retval k_ra8_err_invalid_state     @p session not in `k_ra8_update_stage_staged`.
 * @retval k_ra8_err_invalid_size      `scratch_cap == 0` or invalid `sig_len`/`meta_prefix_len`.
 * @retval k_ra8_err_crc_mismatch      Re-hashed digest mismatched `expected_sha256`.
 * @retval k_ra8_err_validation_failed Signature failed or version downgrade rejected.
 *
 * @pre @p session is in `k_ra8_update_stage_staged`.
 * @pre @p scratch has capacity >= 1 byte.
 * @post On ::k_ra8_ok, `session->verified == 1` and `stage == verified`.
 * @post Rollback counter is NOT modified here when `defer_floor_bump` is true.
 *
 * @note Caller-synchronized per session; default-denies all errors.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_verify(ra8_update_session_t*          session,
                                          const ra8_update_verify_req_t* req,
                                          uint8_t*                       scratch,
                                          uint32_t                       scratch_cap);

/**
 * @brief Atomically activate a verified staged update
 *
 * @details
 * Refuses to execute unless `stage == k_ra8_update_stage_verified` and
 * `verified != 0`. Calls `commit.commit` with `is_trial = (trial_budget > 0)`.
 * When `trial_budget == 0` and `defer_floor_bump == false`, commits the new
 * version floor before transitioning to `confirmed`; otherwise moves to
 * `trial_pending` leaving the OTP/rollback floor untouched until ::ra8_update_confirm.
 *
 * @param[in,out] session     Verified session (non-NULL).
 * @param[in]     seq_or_bank Monotonic sequence number or inactive bank index.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Activated (`confirmed` or `trial_pending`).
 * @retval k_ra8_err_null_ptr      @p session is NULL.
 * @retval k_ra8_err_invalid_state @p session was not verified prior to commit.
 * @retval other                   Error returned by `commit.commit` or `commit_floor`.
 *
 * @pre ::ra8_update_verify succeeded for @p session.
 * @pre @p session is in `k_ra8_update_stage_verified`.
 * @post On ::k_ra8_ok, either `trial_pending` or `confirmed` is entered.
 * @post On hook failure, rollback runs and stage enters `rolled_back`.
 *
 * @note Caller-synchronized per session.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_commit(ra8_update_session_t* session, uint32_t seq_or_bank);

/**
 * @brief Confirm a pending trial update and advance the anti-rollback floor
 *
 * @details
 * Calls `commit.confirm` and advances `rollback.commit_floor` to the verified
 * image version. Calling on an already `confirmed` session is idempotent.
 *
 * @param[in,out] session Trial-pending or confirmed session (non-NULL).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Confirmed and floor updated (or already confirmed).
 * @retval k_ra8_err_null_ptr      @p session is NULL.
 * @retval k_ra8_err_invalid_state @p session neither `trial_pending` nor `confirmed`.
 * @retval other                   Error returned by `commit.confirm` or `commit_floor`.
 *
 * @pre ::ra8_update_commit completed with `trial_budget > 0` (or already confirmed).
 * @pre `session->verified != 0`.
 * @post `session->stage == k_ra8_update_stage_confirmed`.
 * @post Persistent rollback floor is at least `session->verified_version`.
 *
 * @note Caller-synchronized per session.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_confirm(ra8_update_session_t* session);

/**
 * @brief Explicitly abort or roll back @p session recording @p reason
 *
 * @details
 * Invokes `commit.rollback` (if installed), clears `verified`, records
 * @p reason (mapping `k_ra8_ok` to `k_ra8_fail`), and transitions @p session
 * into `k_ra8_update_stage_rolled_back`.
 *
 * @param[in,out] session Initialized session (non-NULL).
 * @param[in]     reason  Failure code triggering rollback.
 *
 * @return ::ra8_err_t The normalized non-ok rollback reason stored on @p session.
 * @retval k_ra8_err_null_ptr @p session is NULL.
 * @retval other              @p reason (or `k_ra8_fail` if `k_ra8_ok` passed).
 *
 * @pre @p session points to an initialized ::ra8_update_session_t.
 * @pre @p reason names the abort condition (`!= k_ra8_ok` preferred).
 * @post `session->verified == 0` and `session->stage == k_ra8_update_stage_rolled_back`.
 * @post Active firmware slot/bank was not left half-committed.
 *
 * @note Caller-synchronized per session.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_update_rollback(ra8_update_session_t* session, ra8_err_t reason);

#ifdef __cplusplus
}
#endif
