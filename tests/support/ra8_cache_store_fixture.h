/**
 * @file ra8_cache_store_fixture.h
 * @brief Bounded media geometry and storage for the cache-store host tests.
 * @details Keeps fixture constants and caller-owned storage separate from the
 * behavioral vectors while preserving one private instance per test binary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/** @brief Deterministic payload seeds used to distinguish stored entries. */
typedef enum : uint8_t {
  k_cs_seed_roundtrip            = 7U,    /**< Put/get round-trip entry.             */
  k_cs_seed_vsource              = 33U,   /**< Entry paged through ra8_vsource.      */
  k_cs_seed_evict_reuse          = 9U,    /**< Entry evicted and then replaced.      */
  k_cs_seed_pinned               = 5U,    /**< Entry pinned during eviction.         */
  k_cs_seed_checkpoint_a         = 11U,   /**< First clean-checkpoint entry.         */
  k_cs_seed_checkpoint_b         = 22U,   /**< Second clean-checkpoint entry.        */
  k_cs_seed_replay_a             = 44U,   /**< First crash-replay entry.             */
  k_cs_seed_replay_b             = 55U,   /**< Second crash-replay entry.            */
  k_cs_seed_post_replay          = 66U,   /**< Entry added after replay.             */
  k_cs_seed_torn_tail            = 0x5AU, /**< Header-less torn-write payload.       */
  k_cs_seed_evicted_before_crash = 12U,   /**< Entry removed before a crash.         */
  k_cs_seed_survives_crash       = 34U,   /**< Entry retained across that crash.     */
  k_cs_seed_bad_header_scan      = 21U,   /**< Valid entry beside malformed headers. */
  k_cs_seed_corrupt_super_base   = 70U,   /**< Base seed for corrupt-super rounds.   */
} cs_seed_t;

/** @brief Sectors and values used to plant malformed log headers. */
typedef enum : uint16_t {
  k_cs_plant_sector_bad_start  = 20U,    /**< Header with mismatched start sector. */
  k_cs_plant_sector_zero_count = 22U,    /**< Header with a zero-sector run.       */
  k_cs_plant_sector_overrun    = 24U,    /**< Header whose run exceeds media.      */
  k_cs_plant_sector_bad_crc    = 26U,    /**< Header with an invalid CRC.          */
  k_cs_plant_start_mismatch    = 999U,   /**< Impossible header start sector.      */
  k_cs_plant_count_past_media  = 60000U, /**< Impossible sector count.             */
} cs_plant_t;

/** @brief Payload sizes selected around the 512-byte sector boundary. */
typedef enum : uint16_t {
  k_cs_bytes_index_filler   = 100,   /**< Single-sector index filler.      */
  k_cs_bytes_sub_sector     = 300,   /**< Sub-sector payload or slice.     */
  k_cs_bytes_post_replay    = 400,   /**< Payload written after replay.    */
  k_cs_bytes_survives_crash = 500,   /**< Payload retained across a crash. */
  k_cs_bytes_two_sector     = 600,   /**< Small two-sector payload.        */
  k_cs_bytes_entry_a        = 700,   /**< Two-sector eviction entry.       */
  k_cs_bytes_replay_b       = 900,   /**< Distinct replay payload length.  */
  k_cs_bytes_evict_reuse    = 1000,  /**< Replacement after eviction.      */
  k_cs_bytes_checkpoint_b   = 1300,  /**< Longer checkpoint entry.         */
  k_cs_bytes_three_sector   = 1500,  /**< Cross-sector partial-tail entry. */
  k_cs_bytes_budget_hog     = 24000, /**< Capacity-exhaustion entry.       */
} cs_bytes_t;

/** @brief Read offsets, keys, and loop limits used by bounded probes. */
typedef enum : uint16_t {
  k_cs_tail_frame_payload_bytes = 476U,  /**< Real bytes in the final frame. */
  k_cs_key_budget_base          = 2000U, /**< First capacity-probe key.      */
  k_cs_budget_put_attempts      = 20U,   /**< Bounded capacity-probe limit.  */
} cs_offset_t;

/** @brief Invalid metadata values used to drive recovery branches. */
typedef enum : uint32_t {
  k_cs_plant_key_base      = 0xDEAD0000U, /**< Planted-header key base. */
  k_cs_super_crc_corrupt   = 0xDEADBEEFU, /**< Invalid superblock CRC.  */
  k_cs_super_magic_corrupt = 0x0BADC0DEU, /**< Invalid super magic.     */
  k_cs_hdr_crc_corrupt     = 0x1234ABCDU, /**< Invalid header CRC.      */
} cs_corrupt_t;

/** @brief Configuration values that initialization must reject. */
typedef enum : uint8_t {
  k_cs_overprovision_pct_rejected = 99U, /**< Above the supported percentage. */
} cs_cfg_reject_t;

/** @brief Geometry and keys shared by the cache-store test vectors. */
typedef enum : uint32_t {
  k_t_index_cap    = 16U,         /**< Index slots.                   */
  k_t_logical      = 200U,        /**< Usable LevelX logical sectors. */
  k_t_sector_bytes = 512U,        /**< LevelX logical-sector size.    */
  k_t_key_a        = 0xA1A1A1A1U, /**< Test key A.                    */
  k_t_key_b        = 0xB2B2B2B2U, /**< Test key B.                    */
  k_t_key_c        = 0xC3C3C3C3U, /**< Test key C.                    */
} t_cs_const_t;

/** @brief Number of independent LevelX control blocks used by the suite. */
typedef enum : uint32_t {
  k_t_flash_pool = 40U, /**< Fresh control blocks available to crash tests. */
} t_pool_const_t;

/** @brief Caller-owned staging sector. */
static uint8_t s_staging[k_t_sector_bytes];
/** @brief Caller-owned cache-store index. */
static ra8_cache_store_entry_t s_index[k_t_index_cap];
/** @brief Fresh LevelX controls for tests that simulate unclean shutdown. */
static LX_NOR_FLASH s_flash_pool[k_t_flash_pool];
/** @brief Index of the next unused LevelX control block. */
static uint32_t s_flash_next;
