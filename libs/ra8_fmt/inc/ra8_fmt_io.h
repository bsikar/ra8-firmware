/**
 * @file ra8_fmt_io.h
 * @brief Format-neutral byte-stream contracts: source, sink, spool, transaction.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * A format engine needs four things from whatever is beneath it: positioned
 * input, append-only output, a sealable scratch artifact, and a durable
 * publication transaction. None of those four is specific to a container
 * format, and none of them is specific to a host: a POSIX file descriptor, a
 * firmware VFS stream, and an in-memory test buffer all bind the same shapes.
 *
 * This header is the transport-neutral and format-neutral half of that
 * contract, so a product under `apps/` can bind it without linking the format
 * tooling under `tools/`. It names no container, includes no decoder, and
 * depends only on ::ra8_err_t.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @typedef ra8_fmt_pread_fn
 * @brief Read @p len bytes at absolute @p offset, reporting the count taken.
 * @details Identical in shape to ::jof_pread_fn, deliberately: the two are the
 * same C type, so a source bound here passes straight to a JOF entry point
 * with no cast and no adapter. Should the two ever drift, every call site that
 * hands ::ra8_fmt_source_t::read_at to the JOF engines stops compiling, which
 * is the intended guard.
 * @since 0.1.0
 */
typedef ra8_err_t (
  *ra8_fmt_pread_fn)(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got);

/** @brief Revalidate one positioned source against its captured immutable view.
 */
typedef ra8_err_t (*ra8_fmt_source_validate_fn)(void* ctx, uint64_t expected_size);

/** @brief Immutable, randomly readable input object. */
typedef struct {
  ra8_fmt_pread_fn           read_at;  /**< Positioned-read callback.    */
  ra8_fmt_source_validate_fn validate; /**< Optional stability callback. */
  void*                      ctx;      /**< Backend-owned context.       */
  uint64_t                   size;     /**< Exact object byte length.    */
} ra8_fmt_source_t;

/** @brief Append text or binary bytes to a bounded backend. */
typedef ra8_err_t (*ra8_fmt_sink_write_fn)(void* ctx, const uint8_t* bytes, size_t len);

/** @brief Injected append-only sink. */
typedef struct {
  ra8_fmt_sink_write_fn write; /**< Exact append callback. */
  void*                 ctx;   /**< Backend-owned context. */
} ra8_fmt_sink_t;

/** @brief Seal an exact scratch artifact for immutable positioned reads. */
typedef ra8_err_t (*ra8_fmt_spool_seal_fn)(void* ctx, uint64_t expected_size);

/** @brief Caller-owned scratch artifact with append, seal, and read seams. */
typedef struct {
  ra8_fmt_pread_fn      read_at; /**< Positioned reader after seal. */
  ra8_fmt_sink_write_fn append;  /**< Append before seal.           */
  ra8_fmt_spool_seal_fn seal;    /**< Seal exact produced bytes.    */
  void*                 ctx;     /**< Backend-owned state.          */
} ra8_fmt_spool_t;

/** @brief Durable artifact-transaction operations. */
typedef struct {
  ra8_fmt_sink_write_fn append;   /**< Append artifact bytes.       */
  ra8_err_t (*commit)(void* ctx); /**< Sync and atomically install. */
  void (*abort)(void* ctx);       /**< Discard owned staging data.  */
} ra8_fmt_transaction_ops_t;

/** @brief One caller-owned artifact transaction. */
typedef struct {
  const ra8_fmt_transaction_ops_t* ops; /**< Transaction implementation. */
  void*                            ctx; /**< Backend-owned state.        */
} ra8_fmt_transaction_t;
