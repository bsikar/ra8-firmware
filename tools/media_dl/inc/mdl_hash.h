/**
 * @file mdl_hash.h
 * @brief Content-identity hashing (FNV-1a 64) for the media downloader's
 *        persistent library state.
 *
 * @details
 * The library-state store (`mdl_state`) records a stable 64-bit identity for
 * every fetched page so a re-run can (a) skip re-downloading a byte-identical
 * image already held -- including one shared across chapters -- and (b) detect a
 * torn file left by a kill mid-write and refetch it rather than package it. The
 * same seam URL-hashes a page's source address so dedup lookup is a cheap
 * fixed-width key rather than a full-string scan.
 *
 * FNV-1a 64 is chosen deliberately: it is a few lines, allocation-free, endian
 * neutral over a byte stream, and already the family the firmware's golden
 * framebuffer checks use -- so a `.mdl_state` written on the host and one a
 * future on-device port writes agree bit for bit. It is a NON-cryptographic
 * identity hash: it defends against accidental corruption and duplication, not
 * against an adversary crafting a collision, which is not part of this threat
 * model (the bytes are already trusted enough to store on disk and read back).
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs.h"
#include "mdl_storage.h"
#include "ra8_err.h"

/** @brief FNV-1a 64-bit constants (the canonical parameters). */
typedef enum : uint64_t {
  k_mdl_fnv_offset          = 0xCBF29CE484222325ULL, /**< FNV-1a 64 offset basis. */
  k_mdl_fnv_prime           = 0x00000100000001B3ULL, /**< FNV-1a 64 prime.        */
  k_mdl_hash_max_file_bytes = 8192000000ULL,         /**< Exact file hash bound.  */
} mdl_fnv_const_t;

/**
 * @brief Continue an FNV-1a 64 fold over a byte range from a running state.
 *
 * @details
 * The streaming primitive: folds @p data into @p seed and returns the new
 * running digest, so a large file can be hashed chunk by chunk with no dynamic
 * allocation. Passing ::k_mdl_fnv_offset as @p seed for the first chunk makes a
 * chunked hash identical to a single-shot ::mdl_hash_bytes of the whole range.
 *
 * @param[in] data First byte of the chunk, or NULL for an empty chunk.
 * @param[in] len  Number of bytes to fold.
 * @param[in] seed Running digest so far (::k_mdl_fnv_offset to start fresh).
 *
 * @return The digest after folding @p data into @p seed.
 * @retval seed When @p len is 0 or @p data is NULL (nothing to fold).
 *
 * @pre @p data, when non-NULL, addresses at least @p len readable bytes.
 * @pre @p seed is either ::k_mdl_fnv_offset or a prior result of this function.
 * @post No argument is modified.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
uint64_t mdl_hash_bytes_seed(const void* data, size_t len, uint64_t seed);

/**
 * @brief FNV-1a 64 hash of a byte range.
 *
 * @details
 * A single-shot fold of @p data starting from ::k_mdl_fnv_offset (it delegates
 * to ::mdl_hash_bytes_seed with that seed). An empty range (`len == 0`) hashes
 * to the offset basis, and a NULL @p data with a non-zero @p len is treated as
 * empty rather than dereferenced.
 *
 * @param[in] data First byte of the range, or NULL for an empty range.
 * @param[in] len  Number of bytes to fold.
 *
 * @return The 64-bit FNV-1a digest.
 * @retval k_mdl_fnv_offset When @p len is 0 or @p data is NULL.
 *
 * @pre @p data, when non-NULL, addresses at least @p len readable bytes.
 * @pre The caller uses the result only as an identity key, not a MAC.
 * @post No argument is modified.
 *
 * @note Thread-safe: depends only on its arguments.
 * @see mdl_hash_bytes_seed
 * @since 0.1.0
 */
uint64_t mdl_hash_bytes(const void* data, size_t len);

/**
 * @brief FNV-1a 64 hash of a NUL-terminated string (excluding the NUL).
 *
 * @details
 * The dedup key for a page's source URL: two runs that scrape the same URL
 * produce the same key, so a page already held is found without re-fetching.
 *
 * @param[in] s String to hash, or NULL.
 *
 * @return The 64-bit FNV-1a digest of @p s's bytes.
 * @retval k_mdl_fnv_offset When @p s is NULL or empty.
 *
 * @pre @p s, when non-NULL, is NUL-terminated.
 * @pre The caller treats the result as an identity key only.
 * @post @p s is not modified.
 *
 * @note Thread-safe: depends only on its argument.
 * @see mdl_hash_bytes
 * @since 0.1.0
 */
uint64_t mdl_hash_str(const char* s);

/**
 * @brief FNV-1a 64 hash of a file's full contents.
 *
 * @details
 * Queries @p path through the injected portable filesystem, rejects non-regular
 * objects before reading, then hashes a regular file up to the implementation's
 * 8.192 GB safety limit through bounded reads and caller-owned scratch. The
 * size is snapshotted before reading; short input, a trailing byte from a
 * concurrent append, or an exhausted read-call ceiling fails rather than
 * publishing a partial-prefix digest. Used to verify a page already on disk
 * still matches the identity recorded in state (a torn file re-hashes to a
 * different value and is refetched) and to record the identity of a freshly
 * fetched page.
 *
 * @param[in,out] storage Initialized filesystem binding and file workspace.
 * @param[in]  path Canonical portable file path (never NULL).
 * @param[out] out  Receives the 64-bit digest on success (never NULL).
 *
 * @return An ::ra8_err_t result.
 * @retval k_ra8_ok              File read fully and hashed; `*out` is set.
 * @retval k_ra8_err_invalid_arg @p path or @p out was NULL, or the path was a
 *                               symlink or non-regular object.
 * @retval k_ra8_err_invalid_size The regular file exceeded the safety bound.
 * @retval k_ra8_err_not_found   The path or a path component did not exist.
 * @retval k_ra8_err_access_denied The process lacked permission to open it.
 * @retval k_ra8_fail            Other open/stat/read/close failure.
 *
 * @pre @p storage, @p path, and @p out are non-NULL.
 * @pre The bound adapter refuses symbolic-link traversal.
 * @pre @p path directly names a readable regular file for success.
 * @post `*out` is written only on ::k_ra8_ok.
 * @post The file position/contents are not modified.
 *
 * @note Not thread-safe against concurrent writers of the same file.
 * @see mdl_hash_bytes
 * @since 0.1.0
 */
ra8_err_t mdl_hash_file(mdl_storage_t* storage, const char* path, uint64_t* out);

/**
 * @brief Hash exactly one snapshotted extent from an already-open stream.
 * @details Reads the declared extent through bounded caller scratch, folds
 * every byte into FNV-1a, then requires an immediate EOF read before publishing
 * the digest. Short input, trailing growth, zero progress, and call-bound
 * exhaustion all fail closed.
 * @param[in,out] file Readable generic file positioned at byte zero.
 * @param[in] expected_size Exact byte extent that must be followed by EOF.
 * @param[out] buffer Caller-owned read scratch.
 * @param[in] buffer_bytes Nonzero extent of @p buffer.
 * @param[out] out Digest written only after the exact extent and EOF are read.
 * @return Canonical read/contract status.
 * @retval k_ra8_ok The exact extent and EOF were observed and hashed.
 * @retval k_ra8_err_invalid_arg A pointer or scratch extent is invalid.
 * @retval k_ra8_err_invalid_size The file/call bound is exceeded.
 * @retval k_ra8_fail The stream shrank, grew, or stopped making progress.
 * @retval other A generic stream read error propagated.
 * @pre @p file is open for reading and positioned at byte zero.
 * @pre @p buffer covers @p buffer_bytes writable bytes and @p out is writable.
 * @post Success leaves @p file positioned at EOF and writes @p out.
 * @post Failure leaves @p out untouched.
 * @note Exposed for transaction validators; ordinary callers use
 *       ::mdl_hash_file.
 * @since 0.1.0
 */
ra8_err_t mdl_hash_stream(fw_fs_file_t* file,
                          uint64_t      expected_size,
                          uint8_t*      buffer,
                          uint32_t      buffer_bytes,
                          uint64_t*     out);
