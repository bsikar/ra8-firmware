/**
 * @file ra8_tz_ipc_attr.h
 * @brief Declarative IPC security / privilege attribution for secure boot
 * @ingroup grp_system
 *
 * @details
 * `ra8_tz_secure_boot_security_init()` takes the two CPSCU attribution words
 * as raw `uint32_t`, so every caller spells its partition as a hex literal and
 * a comment explaining which bits it meant. That is the same shape
 * `ra8_tz_partition.h` replaced on the SAU side, and it has the same failure
 * mode: the bit layout of IPCSAR and IPCPAR is identical but their meaning is
 * not, so one word written into both registers compiles, looks symmetric, and
 * silently makes every channel the Non-Secure world can reach unprivileged as
 * well.
 *
 * This header gives that map one type. An app names, per target, which world
 * may reach it and which privilege level, and the encoding into IPCSAR /
 * IPCPAR lives once. Two separate enums mean the two words cannot be confused
 * for each other, and Secure / Privileged are the zero values, so a
 * zero-initialised descriptor is exactly the chip's cold-reset attribution.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_tz_ipc_world_t
 * @brief Which security world may reach an IPC target (IPCSAR.SAIPC*).
 *
 * @details
 * HUM Ch 3.2.1 "IPCSAR" p 205-207: a set bit hands the target to the
 * Non-Secure world, a clear bit keeps it Secure. Secure is 0 so it is what a
 * zero-initialised descriptor says, matching the cold-reset register value.
 */
typedef enum : uint8_t {
  k_ra8_tz_ipc_world_secure     = 0U, /**< Secure-only (reset default). */
  k_ra8_tz_ipc_world_non_secure = 1U, /**< Reachable from Non-Secure.   */
} ra8_tz_ipc_world_t;

/**
 * @enum ra8_tz_ipc_access_t
 * @brief Which privilege level may reach an IPC target (IPCPAR.PAIPC*).
 *
 * @details
 * HUM Ch 3.2.2 "IPCPAR" p 208-209: a set bit allows Unprivileged access, a
 * clear bit keeps the target Privileged-only. This is a different question
 * from ::ra8_tz_ipc_world_t asked over the same bit positions, which is the
 * whole reason the two are separate types here.
 */
typedef enum : uint8_t {
  k_ra8_tz_ipc_access_privileged   = 0U, /**< Privileged-only (reset default). */
  k_ra8_tz_ipc_access_unprivileged = 1U, /**< Unprivileged access allowed.     */
} ra8_tz_ipc_access_t;

/**
 * @enum ra8_tz_ipc_target_t
 * @brief The eight independently attributable IPC targets.
 *
 * @details
 * These are indices into ::ra8_tz_ipc_attribution_t, not register bits. The
 * bit position each one encodes to is a property of the encoder, so a caller
 * never writes a shift. Ordering follows the register: the two semaphore
 * groups, the two NMI units, then the four channel groups.
 */
typedef enum : uint8_t {
  k_ra8_tz_ipc_target_sem_low   = 0U, /**< IPCSEM0..7.           */
  k_ra8_tz_ipc_target_sem_high  = 1U, /**< IPCSEM8..15.          */
  k_ra8_tz_ipc_target_nmi_unit0 = 2U, /**< IPC0 NMI registers.   */
  k_ra8_tz_ipc_target_nmi_unit1 = 3U, /**< IPC1 NMI registers.   */
  k_ra8_tz_ipc_target_channel0  = 4U, /**< IPC0 channel 0 group. */
  k_ra8_tz_ipc_target_channel1  = 5U, /**< IPC0 channel 1 group. */
  k_ra8_tz_ipc_target_channel2  = 6U, /**< IPC1 channel 0 group. */
  k_ra8_tz_ipc_target_channel3  = 7U, /**< IPC1 channel 1 group. */
  k_ra8_tz_ipc_target_count     = 8U, /**< Entries in a map.     */
} ra8_tz_ipc_target_t;

/**
 * @struct ra8_tz_ipc_target_attr_t
 * @brief The two independent answers for one IPC target.
 */
typedef struct {
  ra8_tz_ipc_world_t  world;  /**< Secure or Non-Secure.       */
  ra8_tz_ipc_access_t access; /**< Privileged or Unprivileged. */
} ra8_tz_ipc_target_attr_t;

/**
 * @struct ra8_tz_ipc_attribution_t
 * @brief Whole-device IPC attribution map, as data.
 *
 * @details
 * Indexed by ::ra8_tz_ipc_target_t. Every target is named, so there is no
 * "the rest keep whatever was there" case to reason about: the descriptor
 * describes the entire pair of registers and the encoder writes both.
 *
 * @invariant Every `target[i].world` is a value of ::ra8_tz_ipc_world_t and
 *            every `target[i].access` a value of ::ra8_tz_ipc_access_t.
 */
typedef struct {
  ra8_tz_ipc_target_attr_t target[k_ra8_tz_ipc_target_count]; /**< Per-target map. */
} ra8_tz_ipc_attribution_t;

/**
 * @brief Encode a descriptor into the IPCSAR / IPCPAR word pair.
 *
 * @details
 * Pure function: it touches no register and is the whole of the bit layout
 * this library knows. Both outputs are written on success and left untouched
 * on failure, so a rejected descriptor cannot half-fill a caller's words.
 *
 * @param[in]  cfg         Attribution map to encode.
 * @param[out] out_ipcsar  Receives the CPSCU.IPCSAR value.
 * @param[out] out_ipcpar  Receives the CPSCU.IPCPAR value.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Both words encoded.
 * @retval k_ra8_err_null_ptr     Any argument is NULL.
 * @retval k_ra8_err_invalid_arg  A target carries a value outside its enum.
 *
 * @pre None; safe to call before the SAU is programmed.
 * @post On success both outputs hold the encoded words.
 * @post On failure neither output is written.
 *
 * @note Thread-safe (pure; no statics). Performs no MMIO.
 * @see ra8_tz_secure_boot_security_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tz_ipc_attribution_encode(const ra8_tz_ipc_attribution_t* cfg,
                                                      uint32_t*                       out_ipcsar,
                                                      uint32_t*                       out_ipcpar);

/**
 * @brief The CPU1 ping-pong partition, as a descriptor.
 *
 * @details
 * The map `cpu1_pingpong_ipc` programmes today, written as what it means: IPC0
 * channel 0 (CPU1 -> CPU0) and IPC1 channel 0 (CPU0 -> CPU1) handed to the
 * Non-Secure world because CPU1 is always-NS, everything else Secure, and
 * every target Privileged-only. Encodes to IPCSAR 0x00050000 / IPCPAR
 * 0x00000000, which is bit for bit the pair that app writes.
 *
 * @param[out] out_cfg  Receives the descriptor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Descriptor written.
 * @retval k_ra8_err_null_ptr  `out_cfg` is NULL.
 *
 * @post On success every one of the ::k_ra8_tz_ipc_target_count entries is
 *       set, including the ones left at the Secure / Privileged default.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tz_ipc_attribution_cpu1_pingpong(ra8_tz_ipc_attribution_t* out_cfg);

/**
 * @brief Run the CPSCU attribution step from a descriptor.
 *
 * @details
 * Encodes `cfg` and forwards to ::ra8_tz_secure_boot_security_init, so the
 * PRCR_S unlock / write / relock sequence stays in one place and this is only
 * the typed door to it. A descriptor that fails to encode is refused before
 * PRCR_S is opened, so a rejected map leaves the write-protect gate closed and
 * CPSCU untouched.
 *
 * @param[in] cfg  Attribution map to programme.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               IPCSAR and IPCPAR written.
 * @retval k_ra8_err_null_ptr     `cfg` is NULL.
 * @retval k_ra8_err_invalid_arg  A target carries a value outside its enum.
 *
 * @pre Caller is in Secure state (CPSCU and PRCR_S are secure-only).
 * @pre Caller has already programmed the SAU partition.
 *
 * @post On success the CPSCU holds the encoded pair and PRCR_S is relocked.
 * @post On failure no register was touched.
 *
 * @note Not thread-safe; runs once at boot.
 * @see ra8_tz_ipc_attribution_encode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_tz_secure_boot_security_init_map(const ra8_tz_ipc_attribution_t* cfg);

#ifdef __cplusplus
}
#endif
