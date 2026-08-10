/**
 * @file ra8_fs_fat_bytes_internal.h
 * @brief The little-endian byte codec and the runtime sector-geometry reads.
 * @ingroup grp_storage
 *
 * @details
 * Two small vocabularies every FAT/exFAT translation unit shares, split out of
 * the alphabetical prototype headers when the 64-bit widening (#676, #683)
 * pushed those against the source-size cap:
 *
 *   - the little-endian field codec (`priv_rd16/32/64`, `priv_wr16/32/64`) the
 *     on-disk structures are read and written through;
 *   - the runtime geometry accessors (`priv_bps`, `priv_cluster_bytes`,
 *     `priv_dir_eps`) that replaced the old compile-time 512-byte-sector
 *     constants -- the sector size is a property of the MEDIUM now.
 *
 * Aggregated by the `ra8_fs_fat_internal.h` umbrella like every other themed
 * sub-header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs_fat_types_internal.h"

/* =============================================================================
 * The sector arena -- four fixed-role bounce buffers (#683)
 * =============================================================================
 *
 * Sector buffers grew from 512 bytes to ::k_ra8_fs_sector_max (4 KiB) when the
 * sector size became a runtime property, and a 4 KiB frame does not belong on
 * a firmware stack budgeted in the low kilobytes. The library is serialised by
 * contract (one lock, no recursion, no reentrancy), so every bounce buffer
 * lives in a small static arena instead, addressed by ROLE. The roles encode
 * the only nesting the call graph performs, so two live buffers can never
 * alias:
 *
 *   - WALK -- a directory/metadata sector held across a scan or a
 *     read-modify-write of a directory entry. Walk holders may call FAT-entry
 *     helpers (which use FAT/FAT2) and leaf I/O helpers (which use IO), and
 *     never each other while their sector is live.
 *   - FAT  -- the sector holding a FAT entry (`priv_fat_get` / the setters).
 *   - FAT2 -- the following sector, live only while a FAT12 entry straddles.
 *   - IO   -- leaf data/bitmap sector I/O: loaded, consumed/patched, written,
 *     dead before any other buffer user runs.
 *
 * A function taking a buffer must finish with it before calling anything that
 * takes the same role -- the discipline every converted call site follows and
 * the reviews on this module enforce.
 */

/**
 * @brief The WALK-role sector buffer (directory scans and entry RMW).
 *
 * @details See the arena discipline above. The pointer is to static storage
 *          of ::k_ra8_fs_sector_max bytes, valid for the whole program; only
 *          LIVENESS is scoped, by the role rules.
 *
 * @return Pointer to the walk-role sector buffer.
 * @retval non-NULL Always.
 *
 * @pre The caller holds the library lock (or none is installed).
 * @pre No other walk-role buffer is live in the current call chain.
 * @post The buffer contents are whatever the previous user left.
 * @post No other state is modified.
 *
 * @note Not thread-safe; the adapter is single-threaded by contract.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t* priv_sec_walk(void);

/**
 * @brief The FAT-role sector buffer (`priv_fat_get` and the FAT setters).
 *
 * @details See the arena discipline above.
 *
 * @return Pointer to the FAT-role sector buffer.
 * @retval non-NULL Always.
 *
 * @pre The caller holds the library lock (or none is installed).
 * @pre No other FAT-role buffer is live in the current call chain.
 * @post The buffer contents are whatever the previous user left.
 * @post No other state is modified.
 *
 * @note Not thread-safe; the adapter is single-threaded by contract.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t* priv_sec_fat(void);

/**
 * @brief The FAT2-role sector buffer (the FAT12 straddle's second sector).
 *
 * @details See the arena discipline above.
 *
 * @return Pointer to the FAT2-role sector buffer.
 * @retval non-NULL Always.
 *
 * @pre The caller holds the library lock (or none is installed).
 * @pre Live only alongside the FAT-role buffer, in the straddle helpers.
 * @post The buffer contents are whatever the previous user left.
 * @post No other state is modified.
 *
 * @note Not thread-safe; the adapter is single-threaded by contract.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t* priv_sec_fat2(void);

/**
 * @brief The IO-role sector buffer (leaf data / bitmap sector transfers).
 *
 * @details See the arena discipline above.
 *
 * @return Pointer to the IO-role sector buffer.
 * @retval non-NULL Always.
 *
 * @pre The caller holds the library lock (or none is installed).
 * @pre No other IO-role buffer is live in the current call chain.
 * @post The buffer contents are whatever the previous user left.
 * @post No other state is modified.
 *
 * @note Not thread-safe; the adapter is single-threaded by contract.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t* priv_sec_io(void);

/**
 * @brief Fill @p n bytes of @p dst with @p value.
 *
 * @details The store-side sibling of ::priv_byte_copy, for zeroing an
 *          arena-taken sector before constructing content in it (the arena
 *          hands back whatever the previous user left, where the old stack
 *          buffers arrived zero-initialised).
 *
 * @param[out] dst   Destination buffer.
 * @param[in]  value Byte value to store.
 * @param[in]  n     Number of bytes to fill.
 *
 * @pre `dst` is non-NULL and holds at least @p n writable bytes.
 * @pre @p n was bounds-checked by the caller.
 * @post `dst[0..n)` all equal @p value.
 * @post No other state is modified.
 *
 * @note Pure store; trivially thread-safe on distinct buffers.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_byte_fill(uint8_t* dst, uint8_t value, uint32_t n);

/**
 * @var k_zero_sector
 * @brief One whole sector of zero bytes, in read-only storage.
 * @details The shared source for every zero-fill write (fresh directory
 *          clusters, cluster tails). Const, so it costs code memory rather
 *          than SRAM, and one copy serves every translation unit.
 * @note Read-only; never cast away the const.
 * @warning Writing through a cast pointer would corrupt every zero-fill.
 * @since 0.1.0
 */
extern const uint8_t k_zero_sector[k_ra8_fs_sector_max];

/**
 * @brief Decode a little-endian uint16_t from a byte buffer.
 *
 * @details Trivial little-endian byte assembler. Avoids `memcpy` so
 *          clang-tidy's strict-alias check stays happy.
 *
 * @param[in] p Pointer to two bytes.
 *
 * @return The decoded value.
 * @retval 0..UINT16_MAX  Value assembled from `p[0]` and `p[1]`.
 *
 * @pre `p` is non-NULL and points to at least 2 readable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post No state modified.
 * @post Result equals `p[0] | (p[1] << 8)`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint16_t priv_rd16(const uint8_t* p);

/**
 * @brief Decode a little-endian uint32_t from a byte buffer.
 *
 * @details Trivial little-endian byte assembler for 4 bytes.
 *
 * @param[in] p Pointer to four bytes.
 *
 * @return The decoded value.
 * @retval 0..UINT32_MAX  Value assembled from `p[0..3]`.
 *
 * @pre `p` is non-NULL and points to at least 4 readable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post No state modified.
 * @post Result equals `p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_rd32(const uint8_t* p);

/**
 * @brief Decode a little-endian uint64_t from a byte buffer.
 *
 * @details Trivial little-endian byte assembler for 8 bytes: the width of
 *          exFAT's `DataLength` / `ValidDataLength` and of a GPT entry's LBAs.
 *
 * @param[in] p Pointer to eight bytes.
 *
 * @return The decoded value.
 * @retval 0..UINT64_MAX  Value assembled from `p[0..7]`.
 *
 * @pre `p` is non-NULL and points to at least 8 readable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post No state modified.
 * @post Result equals the little-endian 64-bit value at `p`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint64_t priv_rd64(const uint8_t* p);

/**
 * @brief One mounted volume's sector size in bytes.
 *
 * @details The runtime `m->bytes_per_sector`, returned through one accessor so
 *          every arithmetic site reads the same way. A power of two in
 *          ::k_ra8_fs_sector_min..::k_ra8_fs_sector_max on any mounted volume.
 *
 * @param[in] m Mounted volume.
 *
 * @return Sector size in bytes.
 * @retval 512..4096 The volume's sector size.
 *
 * @pre `m` is non-NULL and parsed (`bytes_per_sector` populated).
 * @pre The mount validated the size against the backend's block size.
 * @post No state modified.
 * @post Result is a power of two.
 *
 * @note Pure read; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_bps(const ra8_fs_mount_t* m);

/**
 * @brief One mounted volume's cluster size in bytes.
 *
 * @details `sectors_per_cluster * bytes_per_sector`. Fits 32 bits on every
 *          legal volume: exFAT caps a cluster at 32 MiB, FAT at 64 sectors of
 *          4096 bytes.
 *
 * @param[in] m Mounted volume.
 *
 * @return Cluster size in bytes.
 * @retval 512..33554432 The volume's cluster size.
 *
 * @pre `m` is non-NULL and parsed (geometry fields populated).
 * @pre The volume's cluster geometry is legal for its type.
 * @post No state modified.
 * @post Result is a power of two.
 *
 * @note Pure read; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_cluster_bytes(const ra8_fs_mount_t* m);

/**
 * @brief Directory entries per sector on one mounted volume.
 *
 * @details `bytes_per_sector / 32` -- 16 on a 512-byte volume, 128 on 4Kn.
 *          Replaces the old compile-time constant, which baked 512 in.
 *
 * @param[in] m Mounted volume.
 *
 * @return 32-byte directory entries per sector.
 * @retval 16..128 Entries per sector.
 *
 * @pre `m` is non-NULL and parsed (`bytes_per_sector` populated).
 * @pre `bytes_per_sector` is a multiple of 32.
 * @post No state modified.
 * @post Result times 32 equals the sector size.
 *
 * @note Pure read; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_dir_eps(const ra8_fs_mount_t* m);

/**
 * @brief Encode a little-endian uint16_t into a byte buffer.
 *
 * @details Inverse of `priv_rd16`. Writes the low byte first.
 *
 * @param[out] p Pointer to two writable bytes.
 * @param[in]  v Value to encode.
 *
 * @pre `p` is non-NULL and points to at least 2 writable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post `p[0]` and `p[1]` reflect the little-endian encoding of `v`.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_wr16(uint8_t* p, uint16_t v);

/**
 * @brief Encode a little-endian uint32_t into a byte buffer.
 *
 * @details Inverse of `priv_rd32`. Writes lowest byte first.
 *
 * @param[out] p Pointer to four writable bytes.
 * @param[in]  v Value to encode.
 *
 * @pre `p` is non-NULL and points to at least 4 writable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post `p[0..3]` reflect the little-endian encoding of `v`.
 * @post No other state modified.
 *
 * @note Trivially thread-safe; not reentrant against the same buffer.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_wr32(uint8_t* p, uint32_t v);

/**
 * @brief Encode a uint64_t into a byte buffer, little-endian.
 *
 * @details The 8-byte companion of ::priv_wr32, for exFAT's 64-bit
 *          `DataLength` / `ValidDataLength` fields and the formatter's
 *          `PartitionOffset` / `VolumeLength`.
 *
 * @param[out] p Pointer to eight writable bytes.
 * @param[in]  v Value to store.
 *
 * @pre `p` is non-NULL and points to at least 8 writable bytes.
 * @pre Caller has bounds-checked `p`.
 * @post `p[0..7]` hold @p v little-endian.
 * @post No other state modified.
 *
 * @note Pure store; trivially thread-safe on distinct buffers.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_wr64(uint8_t* p, uint64_t v);
