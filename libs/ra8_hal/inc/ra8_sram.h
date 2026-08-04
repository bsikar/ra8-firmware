/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sram.h
 * @brief SRAM (with ECC) HAL driver public API
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Full HUM Ch 58 (p 3527-3541) coverage of the on-chip SRAM. The
 * RA8D2 has 2 MB of SRAM split across four banks
 * (SRAM0..SRAM2 = 512 KB each, SRAM3 = 128 KB) with optional SEC-DED
 * (64-bit data + 8-bit syndrome) ECC.
 *
 * Public surface:
 *
 *  - **Lifecycle**: ``ra8_sram_init`` / ``ra8_sram_deinit``,
 *    ``ra8_sram_enter_stop`` / ``ra8_sram_exit_stop`` per bank.
 *  - **Mode**: per-bank ECC mode + OAD via ``ra8_sram_set_mode``,
 *    1-bit latch via ``cfg.enable_1bit_latch``, ECC region size via
 *    ``ra8_sram_set_eccrgn``.
 *  - **Wait state**: ``ra8_sram_set_wait_state_for_clock`` per the
 *    HUM Ch 58.3.7 thresholds -- called by ``ra8_cgc_init``, not by
 *    applications; see that function for why it takes no raw bool.
 *  - **TrustZone**: ``ra8_sram_set_security``,
 *    ``ra8_sram_set_ecc_security``, ``ra8_sram_set_boundary``.
 *  - **Status**: ``ra8_sram_get_status``, ``ra8_sram_clear_status``,
 *    ``ra8_sram_clear_address``.
 *  - **Zero-init**: ``ra8_sram_zero_init_bank`` per HUM Ch 58.3.2 to
 *    write deterministic ECC syndromes before enabling ``with-check``.
 *  - **ECC self-test**: ``ra8_sram_self_test`` -- runs the
 *    HUM Ch 58.3.4 decoder-test flowchart.
 *  - **Error handler**: global ``ra8_sram_attach_handler`` and per-bank
 *    ``ra8_sram_attach_bank_handler``; both fan-outs invoked by
 *    ``ra8_sram_dispatch`` (test path) and
 *    ``ra8_sram_dispatch_from_esr`` (NMI path that snapshots SRAMESR
 *    + EAR and walks every bit).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_sram_regs.h"

/* =============================================================================
 * Configuration types
 * =============================================================================
 */

/**
 * @enum ra8_sram_ecc_mode_t
 * @brief ECC operating mode for a single SRAM bank.
 *
 * @details
 * Mirrors ``SRAMCRn.ECCMOD[1:0]`` (HUM Ch 58.2.7 p 3532). The
 * ``01b`` encoding is documented as "Setting prohibited" and is not
 * exposed.
 */
typedef enum : uint8_t {
  k_ra8_sram_ecc_disabled = 0U, /**< ECC off (00b). Default at reset.              */
  k_ra8_sram_ecc_no_check = 1U, /**< ECC encode/correct, no error reporting (10b). */
  k_ra8_sram_ecc_with_chk = 2U, /**< Full ECC (11b) -- raises NMI / reset.         */
} ra8_sram_ecc_mode_t;

/**
 * @enum ra8_sram_on_error_t
 * @brief Action taken when ECC error is detected (mirrors SRAMCRn.OAD).
 *
 * @details
 * Per HUM Ch 58.2.7 "OAD bit" p 3533: 0=NMI, 1=Reset.
 */
typedef enum : uint8_t {
  k_ra8_sram_on_error_interrupt = 0U, /**< Raise NMI on ECC error.     */
  k_ra8_sram_on_error_reset     = 1U, /**< Reset the MCU on ECC error. */
} ra8_sram_on_error_t;

/**
 * @enum ra8_sram_eccrgn_size_t
 * @brief Public form of ``SRAMECCRGNn.ECCRGN`` (per-bank ECC region size).
 *
 * @details
 * Per HUM Ch 58.2.8..58.2.11, p 3533-3535. SRAM0..SRAM2 step in 128
 * KB increments up to 512 KB. SRAM3 only ever holds the 128 KB
 * encoding (any larger value is "Setting prohibited" for that bank).
 */
typedef enum : uint8_t {
  k_ra8_sram_region_off   = 0U, /**< 000b -- no ECC region in this bank. */
  k_ra8_sram_region_128kb = 1U, /**< 001b -- first 128 KB.               */
  k_ra8_sram_region_256kb = 2U, /**< 010b -- first 256 KB.               */
  k_ra8_sram_region_384kb = 3U, /**< 011b -- first 384 KB.               */
  k_ra8_sram_region_512kb = 4U, /**< 100b -- whole 512 KB bank.          */
} ra8_sram_eccrgn_size_t;

/**
 * @struct ra8_sram_bank_cfg_t
 * @brief Per-bank ECC configuration descriptor.
 *
 * @details
 * One of these is filled in for each bank the caller wants to enable
 * ECC on. Defaults to ``ecc_disabled`` so unfilled banks are no-ops.
 */
typedef struct {
  ra8_sram_ecc_mode_t    ecc_mode;          /**< ECC mode (off / no-check / with-check).  */
  ra8_sram_on_error_t    on_error;          /**< NMI vs Reset on ECC error.               */
  bool                   enable_1bit_latch; /**< Set ``E1STSEN``: latch 1-bit errors.     */
  ra8_sram_eccrgn_size_t eccrgn;            /**< ECC target region size (SRAMECCRGNn).    */
  bool                   zero_init;         /**< If true, init() runs the zero-init pass. */
} ra8_sram_bank_cfg_t;

/**
 * @struct ra8_sram_security_cfg_t
 * @brief TrustZone security attribution configured at init time.
 *
 * @details
 * Mirrors HUM Ch 58.2.2 "SRAMSAR" (p 3528) and HUM Ch 58.2.3
 * "SRAMESAR" (p 3529). Each per-bank flag, when ``true``, marks
 * the corresponding SRAM register set as Non-Secure. ``wtsc_ns``
 * marks SRAMWTSC as Non-Secure. ``ecc_region_ns`` marks the whole
 * ECC region (across all banks) Non-Secure via SRAMESAR.
 *
 * Boundary addresses for SRAMSABARn live in
 * ``boundary_offset[bank]`` -- one absolute offset per bank, 4 KB
 * aligned per HUM Ch 58.2.1 (p 3527).
 */
typedef struct {
  bool     bank_ns[k_ra8_sram_bank_count];         /**< Bit n -> SRAMSAn = 1 (NS). */
  bool     wtsc_ns;                                /**< SRAMSAR.SRAMWTSA bit.      */
  bool     ecc_region_ns;                          /**< SRAMESAR.SRAMESA bit.      */
  uint32_t boundary_offset[k_ra8_sram_bank_count]; /**< SRAMSABARn boundary value. */
} ra8_sram_security_cfg_t;

/**
 * @struct ra8_sram_config_t
 * @brief Top-level driver configuration.
 *
 * @details
 * ``banks[i]`` is applied to SRAM bank ``i`` (0..3). Banks for which
 * ``ecc_mode == k_ra8_sram_ecc_disabled`` simply ungate the module
 * clock and clear SRAMCRn -- nothing more.
 *
 * ``apply_security`` selects whether the CPSCU / SRAMSAR / SRAMESAR
 * registers are touched at all. Leaving it ``false`` keeps the
 * post-reset (all-Secure) layout.
 *
 * There is deliberately no wait-state field. SRAMWTSC.WTEN is not a
 * per-application preference: HUM Ch 58.3.7 "Wait State" p 3540 makes
 * it a function of ICLK alone, and getting it wrong is not a
 * performance choice but undefined operation. ``ra8_cgc_init`` owns it
 * and programs it from the ICLK it is about to select, in the same
 * window as the MRAM wait-state latches. A ``wait_state`` member here
 * would let a zero-initialised config silently clear it after boot,
 * which is exactly how the two ECC demos in this tree were re-breaking
 * their own memory system (tracker #524).
 */
typedef struct {
  ra8_sram_bank_cfg_t     banks[k_ra8_sram_bank_count]; /**< Per-bank settings.             */
  ra8_sram_security_cfg_t security;                     /**< CPSCU security attribution.    */
  bool                    apply_security;               /**< Touch CPSCU registers if true. */
} ra8_sram_config_t;

/* =============================================================================
 * Status / event types
 * =============================================================================
 */

/**
 * @struct ra8_sram_status_t
 * @brief Snapshot of ECC error state (filled by ``ra8_sram_get_status``).
 *
 * @details
 * Bits in ``raw_esr`` follow the SRAMESR layout exactly (HUM Ch 58.2.12
 * p 3535) -- see ``ra8_sram_esr_bit_t`` in ``ra8_sram_regs.h``. The
 * decoded helpers ``one_bit_mask`` / ``two_bit_mask`` collapse those 8
 * bits into a per-bank bitmap (bit i = bank i).
 *
 * ``addr_1bit[bank]`` / ``addr_2bit[bank]`` carry the absolute
 * faulting address (EAR offset + 0x2200_0000), or 0 if no error has
 * fired for that bank/slot.
 */
typedef struct {
  uint16_t  raw_esr;                          /**< Raw SRAMESR value.                     */
  uint8_t   one_bit_mask;                     /**< Bit i = SRAMi 1-bit error.             */
  uint8_t   two_bit_mask;                     /**< Bit i = SRAMi 2-bit error.             */
  uintptr_t addr_1bit[k_ra8_sram_bank_count]; /**< Captured 1-bit error address per bank. */
  uintptr_t addr_2bit[k_ra8_sram_bank_count]; /**< Captured 2-bit error address per bank. */
} ra8_sram_status_t;

/**
 * @struct ra8_sram_bank_info_t
 * @brief Static description of one bank.
 *
 * @details
 * Returned by ``ra8_sram_get_bank_info`` so callers do not have to
 * carry the layout constants from ``ra8_sram_regs.h``.
 */
typedef struct {
  uint8_t   bank;      /**< Bank index 0..3.                      */
  uintptr_t data_base; /**< Absolute Secure base of data region.  */
  uint32_t  data_size; /**< Bytes in the data region.             */
  uintptr_t ecc_base;  /**< Absolute Secure base of ECC syndrome. */
  uint32_t  ecc_size;  /**< Bytes in the ECC syndrome region.     */
} ra8_sram_bank_info_t;

/**
 * @typedef ra8_sram_error_fn_t
 * @brief ECC error callback signature.
 * @param[in] ctx       Caller-supplied opaque context pointer.
 * @param[in] bank      Bank index 0..3 that raised the error.
 * @param[in] is_2bit   ``true`` for 2-bit (uncorrectable), ``false`` for 1-bit.
 * @param[in] err_addr  Faulting offset from the SRAM bank base.
 */
typedef void (*ra8_sram_error_fn_t)(void* ctx, uint8_t bank, bool is_2bit, uintptr_t err_addr);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the SRAM driver and configure each bank.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Driver initialized, all banks programmed.
 * @retval k_ra8_err_null_ptr      ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg   A bank carries an out-of-range mode value.
 * @retval k_ra8_err_hw_init_failed MSTP ungate failed for one of the banks.
 *
 * @pre  ``ra8_mstp_init`` has run.
 * @pre  Caller is in single-threaded init context (writes to SRAMPRCR_S
 *       are not thread-safe).
 * @post Each bank's MSTPCRA bit is cleared.
 * @post Each bank's SRAMCRn matches the requested ``ecc_mode`` /
 *       ``on_error`` / ``enable_1bit_latch`` / ``eccrgn``.
 *
 * @par Init Sequence:
 *   1. Validate every bank cfg (mode + region in range).
 *   2. ``ra8_mstp_enable`` per bank (HUM 58.3.1 p 3538).
 *   3. Optionally apply ``security`` cfg via SRAMSAR / SRAMESAR / SRAMSABARn.
 *   4. For each bank requested in ``zero_init``, run the deterministic
 *      ECC zero pass (HUM 58.3.2) before any with-check mode.
 *   5. Apply per-bank SRAMECCRGNn and SRAMCRn under SRAMPRCR_S unlock.
 *   6. Clear any latched SRAMESR / EAR state.
 *
 *   SRAMWTSC is not in that list: it belongs to ``ra8_cgc_init``.
 *
 * @note Not thread-safe.
 * @warning Per HUM 58.2.7 p 3533, enabling ``ecc_with_chk`` on
 *          uninitialized SRAM can immediately trigger a spurious NMI
 *          or reset. Set ``zero_init = true`` for any bank where
 *          ``ecc_mode == k_ra8_sram_ecc_with_chk``.
 *
 * @see ra8_sram_deinit
 * @see ra8_sram_zero_init_bank
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_init(const ra8_sram_config_t* cfg);

/**
 * @brief Tear down the driver and re-gate each bank.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre  Caller has drained any in-flight DMA targeting the SRAM banks.
 * @pre  ``ra8_sram_init`` has been called previously (deinit is idempotent
 *       even if not, but the log line says "not initialized" in that case).
 * @post Each bank's SRAMCRn is cleared (ECC disabled).
 * @post Each bank's MSTPCRA bit is set (peripheral clock-gated).
 *
 * @note Not thread-safe.
 * @see ra8_sram_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_deinit(void);

/**
 * @brief Re-gate a single bank's clock (Module-Stop entry).
 *
 * @param[in] bank Bank index 0..3.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Bank gated.
 * @retval k_ra8_err_invalid_arg  ``bank`` >= ``k_ra8_sram_bank_count``.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  No CPU is currently accessing the bank (HUM 58.3.1 p 3538).
 * @post MSTPCRA bit for the bank is set.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_enter_stop(uint8_t bank);

/**
 * @brief Re-ungate a single bank's clock (Module-Stop exit).
 *
 * @param[in] bank Bank index 0..3.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Bank ungated.
 * @retval k_ra8_err_invalid_arg  ``bank`` >= ``k_ra8_sram_bank_count``.
 *
 * @pre  Bank is currently in the Module-Stop state (or never entered).
 * @pre  Caller has not yet started access to the bank.
 * @post MSTPCRA bit for the bank is cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_exit_stop(uint8_t bank);

/* =============================================================================
 * ECC mode set / status / clear
 * =============================================================================
 */

/**
 * @brief Change the ECC mode of a single bank at runtime.
 *
 * @param[in] bank Bank index 0..3.
 * @param[in] cfg  Non-NULL bank configuration.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Bank reprogrammed.
 * @retval k_ra8_err_null_ptr      ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg   ``bank`` >= ``k_ra8_sram_bank_count``
 *                                 or ``cfg->ecc_mode`` invalid.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  Bank's MSTPCRA bit is already clear.
 * @post SRAMCRn for ``bank`` reflects ``cfg``.
 * @post SRAMECCRGNn for ``bank`` reflects ``cfg->eccrgn``.
 * @post SRAMPRCR_S is re-locked on return.
 *
 * @note Not thread-safe. Does NOT run the zero-init pass; call
 *       ``ra8_sram_zero_init_bank`` first if switching to with-check
 *       mode on an uninitialized region.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_set_mode(uint8_t bank, const ra8_sram_bank_cfg_t* cfg);

/**
 * @brief Update only the ECC region size (SRAMECCRGNn) for one bank.
 *
 * @param[in] bank   Bank index 0..3.
 * @param[in] region One of ``ra8_sram_eccrgn_size_t``.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Region size applied.
 * @retval k_ra8_err_invalid_arg  Bank or region out of range, or bank 3
 *                               with a region > 128 KB.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  ``region`` matches the HUM Ch 58.2.8..58.2.11 encoding.
 * @post SRAMECCRGNn[bank] reflects ``region``.
 * @post SRAMPRCR_S is re-locked on return.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_set_eccrgn(uint8_t bank, ra8_sram_eccrgn_size_t region);

/**
 * @brief Derive SRAMWTSC.WTEN from an ICLK frequency and program it.
 *
 * @details
 * The ONLY way this driver writes SRAMWTSC, and deliberately so: the
 * register has exactly one correct value at a given ICLK, so a raw
 * `set(bool)` setter would only ever be a way to get it wrong.
 *
 * HUM Ch 58.3.7 "Wait State" p 3540 states the rule and the stakes:
 * above half the rated maximum ICLK a wait cycle must be inserted, and
 * "when the wait is not inserted, the operation is not guaranteed". The
 * observed failure is not a hang -- it is a single bit dropped out of a
 * value read back from SRAM at full speed, silently, with the memory
 * itself intact (tracker #524, and the Ethernet TX frame corruption of
 * #499, which is the same fault seen through the GWCA's DMA reads).
 *
 * Called from ``ra8_cgc_init`` before the SCKSCR switch that raises
 * ICLK, so no code ever executes in the unguaranteed window.
 *
 * @param[in] iclk_hz       ICLK frequency in Hz, > 0.
 * @param[in] iclk_max_hz   Rated maximum ICLK for the part variant
 *                          (250e6, 200e6, or 150e6 -- HUM 58.3.7); on
 *                          RA8D2 use ``k_ra8_iclk_max_hz``.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               WTEN written.
 * @retval k_ra8_err_invalid_arg  Either argument is zero.
 *
 * @pre  The SRAM control window is reachable (it is out of reset).
 * @pre  ``iclk_max_hz`` matches the part-specific datasheet.
 * @post WTEN=1 iff ``iclk_hz > iclk_max_hz / 2``.
 * @post SRAMPRCR_S is re-locked on return.
 *
 * @note Not thread-safe; boot context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_set_wait_state_for_clock(uint32_t iclk_hz, uint32_t iclk_max_hz);

/**
 * @brief Snapshot the current ECC error state across all four banks.
 *
 * @param[out] out Non-NULL status receiver.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Status copied to ``*out``.
 * @retval k_ra8_err_null_ptr  ``out`` was NULL.
 *
 * @pre  ``out`` points to writable memory of at least
 *       ``sizeof(ra8_sram_status_t)`` bytes.
 * @pre  Reads from SRAMESR / SRAMEAR are non-destructive.
 * @post ``out->raw_esr`` matches the live SRAMESR.
 * @post ``out->one_bit_mask`` / ``two_bit_mask`` decode the same value.
 *
 * @note Thread-safe with respect to the hardware (read-only path).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_get_status(ra8_sram_status_t* out);

/**
 * @brief Clear the listed ECC error bits in SRAMESR (also clears EAR).
 *
 * @param[in] esr_mask Bitmask in SRAMESR encoding (see
 *                     ``ra8_sram_esr_bit_t``).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Mask written to SRAMESCLR.
 * @retval k_ra8_err_invalid_arg  ``esr_mask`` includes reserved bits.
 *
 * @pre  ``ra8_sram_init`` has run (the register is not PRCR-protected
 *       so the precondition is just module-level state).
 * @pre  ``esr_mask`` fits in the low 8 bits of SRAMESCLR.
 * @post Bits set in ``esr_mask`` are cleared in SRAMESR on read-back.
 * @post SRAMEAR for the corresponding bank/slot is also cleared
 *       (per HUM 58.2.13 p 3536).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_clear_status(uint16_t esr_mask);

/**
 * @brief Clear the captured EAR address for a single (bank, slot) pair.
 *
 * @param[in] bank Bank index 0..3.
 * @param[in] slot ``k_ra8_sram_ear_slot_1bit`` or
 *                 ``k_ra8_sram_ear_slot_2bit``.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               EAR cleared via the matching SRAMESCLR bit.
 * @retval k_ra8_err_invalid_arg  Bank or slot out of range.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  Caller already drained the corresponding callback so a stale
 *       address is acceptable.
 * @post SRAMEAR[bank][slot] reads back as 0.
 * @post SRAMESR.ERR{bank}{slot} is cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_clear_address(uint8_t bank, uint8_t slot);

/* =============================================================================
 * Zero-init / self-test / introspection
 * =============================================================================
 */

/**
 * @brief Deterministically zero one bank under ECC-no-check.
 *
 * @details
 * Per HUM Ch 58.3.2 "Correction of ECC Errors", p 3538: SRAM contents
 * are undefined after power-on, so reading any address with
 * ``ECCMOD=11b`` will fire spurious 2-bit errors. This routine puts
 * the bank into ``ECCMOD=10b`` (encode but do not check), writes a
 * 64-bit zero across every word so each line carries a valid
 * syndrome, then leaves the bank in ECC-disabled mode so the caller
 * can pick the final mode via ``ra8_sram_set_mode``.
 *
 * The fill stride is 8 bytes (HUM 58.4.2 p 3541 -- "SRAM are read in
 * 8-byte (64-bit) units"), and the bank size comes from
 * ``ra8_sram_bank_size_bytes`` so SRAM3's smaller 128 KB region is
 * handled correctly.
 *
 * @param[in] bank Bank index 0..3.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Bank zeroed.
 * @retval k_ra8_err_invalid_arg  Bank out of range.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  No live data lives in this bank (the routine overwrites
 *       everything).
 * @post Every 64-bit word in the bank holds ``0x00000000_00000000``.
 * @post Bank is left in ``ECCMOD=00b`` (ECC disabled) -- caller
 *       reprograms via ``ra8_sram_set_mode`` if a different mode is
 *       desired.
 *
 * @note Not thread-safe. Holds SRAMPRCR_S unlocked across two writes.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_zero_init_bank(uint8_t bank);

/**
 * @brief Run the HUM Ch 58.3.4 ECC decoder self-test on one bank.
 *
 * @details
 * Implements the eight-step flowchart from HUM p 3539:
 *
 *   1. Unlock SRAMPRCR_S.
 *   2. SRAMCRn = 0x08 (ECC, no check, bypass off).
 *   3. Write 8 bytes of seed data at ``probe_offset``.
 *   4. SRAMCRn = 0x80 (ECC off, bypass on) -- raw syndrome readable.
 *   5. Read syndrome, XOR ``inject_mask`` to corrupt 1 or 2 bits, write back.
 *   6. SRAMCRn = 0x1C (ECC + check, E1STSEN=1, bypass off).
 *   7. Read the probed line.
 *   8. Confirm SRAMESR.ERR{bank}{0|1} latched.
 *
 * Each step is preceded by a Data Memory Barrier on the real chip;
 * under ``RA8_OFF_TARGET`` the barrier is a no-op.
 *
 * @param[in]  bank          Bank index 0..3.
 * @param[in]  probe_offset  Offset from the bank base, 8-byte aligned,
 *                           strictly less than the bank size.
 * @param[in]  inject_two_bit ``true`` to flip 2 bits (uncorrectable),
 *                            ``false`` to flip 1 bit (correctable).
 * @param[out] out_caught     ``true`` if SRAMESR latched the expected flag.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Self-test ran to completion.
 * @retval k_ra8_err_invalid_arg  Bank/offset/out_caught invalid.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  Bank is not currently being used by the application; the
 *       sequence corrupts ``probe_offset`` deliberately.
 * @post SRAMCRn is left in 0x1C (ECC + check + 1-bit latch) so the
 *       application can re-claim the bank.
 * @post ``*out_caught`` reflects whether SRAMESR latched the error.
 *
 * @note Not thread-safe. Designed for boot-time / diagnostic use.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sram_self_test(uint8_t bank, uint32_t probe_offset, bool inject_two_bit, bool* out_caught);

/**
 * @brief Report the static layout of one bank.
 *
 * @param[in]  bank Bank index 0..3.
 * @param[out] out  Non-NULL destination.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Layout filled.
 * @retval k_ra8_err_invalid_arg  Bank out of range.
 * @retval k_ra8_err_null_ptr     ``out`` was NULL.
 *
 * @pre  ``out`` points to writable storage of size ``ra8_sram_bank_info_t``.
 * @pre  ``bank`` < ``k_ra8_sram_bank_count``.
 * @post ``out->bank == bank``.
 * @post ``out->data_size`` matches HUM Ch 58.1 Table 58.1 p 3527.
 *
 * @note Side-effect free.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_get_bank_info(uint8_t bank, ra8_sram_bank_info_t* out);

/* =============================================================================
 * TrustZone security attribution
 * =============================================================================
 */

/**
 * @brief Write SRAMSAR (per-bank register security + WTSC security).
 *
 * @param[in] sa_mask Bitmask of ``ra8_sram_sar_bit_t`` values.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               SAR register updated.
 * @retval k_ra8_err_invalid_arg  ``sa_mask`` includes undefined bits.
 *
 * @pre  Caller is in Secure World (HUM 58.2.2 p 3528 -- "Only Secure
 *       access can write to this register").
 * @pre  PRCR_S.PRC4 has been unlocked by upstream code (SRAM CPSCU
 *       writes are gated on PRC4, not SRAMPRCR_S).
 * @post SRAMSAR reads back ``sa_mask``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_set_security(uint32_t sa_mask);

/**
 * @brief Write SRAMESAR (ECC region security).
 *
 * @param[in] non_secure ``true`` -> SRAMESA=1 (NS); ``false`` -> Secure.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre  Caller is in Secure World (HUM 58.2.3 p 3529).
 * @pre  PRCR_S.PRC4 has been unlocked by upstream code.
 * @post SRAMESAR reads back the requested attribute.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_set_ecc_security(bool non_secure);

/**
 * @brief Write SRAMSABARn (per-bank Secure/Non-Secure boundary).
 *
 * @param[in] bank   Bank index 0..3.
 * @param[in] offset Boundary offset inside the bank's 0x80000-byte
 *                   slot (must be 4 KB aligned per HUM 58.2.1 p 3527).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               SABAR updated.
 * @retval k_ra8_err_invalid_arg  Bank or offset rejected.
 *
 * @pre  Caller is in Secure World.
 * @pre  ``offset & k_ra8_sram_sabar_align_mask == 0``.
 * @post SRAMSABARn reads back ``offset & ~k_ra8_sram_sabar_align_mask``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_set_boundary(uint8_t bank, uint32_t offset);

/* =============================================================================
 * Interrupt / fault path
 * =============================================================================
 */

/**
 * @brief Attach the global ECC error callback.
 *
 * @param[in] fn  Non-NULL handler invoked from the NMI ISR (or test code).
 * @param[in] ctx Opaque context pointer forwarded to the handler.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Handler installed.
 * @retval k_ra8_err_null_ptr  ``fn`` was NULL.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  Caller writes shared data the handler reads under a memory
 *       barrier (the handler runs at NMI priority).
 * @post Subsequent calls to ``ra8_sram_dispatch`` will fire ``fn``.
 *
 * @note Not thread-safe at install time.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sram_attach_handler(ra8_sram_error_fn_t fn, void* ctx);

/**
 * @brief Attach a per-bank ECC error callback.
 *
 * @param[in] bank Bank index 0..3.
 * @param[in] fn   Non-NULL handler.
 * @param[in] ctx  Opaque context pointer.
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Handler installed for this bank only.
 * @retval k_ra8_err_null_ptr     ``fn`` was NULL.
 * @retval k_ra8_err_invalid_arg  Bank out of range.
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  Bank handlers are independent of the global handler -- both
 *       fire if both are attached.
 * @post Subsequent ``ra8_sram_dispatch`` / ``ra8_sram_dispatch_from_esr``
 *       call ``fn(ctx, bank, ...)``.
 *
 * @note Not thread-safe at install time.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_sram_attach_bank_handler(uint8_t bank, ra8_sram_error_fn_t fn, void* ctx);

/**
 * @brief Dispatch an ECC error event to the registered handler.
 *
 * @param[in] bank     Bank index 0..3 (out-of-range silently ignored).
 * @param[in] is_2bit  ``true`` for uncorrectable, ``false`` for 1-bit.
 * @param[in] err_addr Faulting offset (from SRAMEARnm).
 *
 * @details
 * Fires both the global handler (if attached) and the per-bank handler
 * (if attached). Out-of-range bank is silently ignored so the caller
 * can blindly walk SRAMESR bits.
 *
 * @pre  ``bank`` is a valid bank index OR the call is ignored.
 * @pre  Handler is called with the same ``ctx`` supplied to
 *       ``ra8_sram_attach_handler``.
 * @post Both attached handlers run to completion before this function
 *       returns.
 *
 * @note Safe to call from NMI context (the underlying handler
 *       must itself be NMI-safe).
 * @since 0.1.0
 *
 * @post Caller-visible state matches the documented contract.
 */
void ra8_sram_dispatch(uint8_t bank, bool is_2bit, uintptr_t err_addr);

/**
 * @brief Read SRAMESR + EAR and dispatch every latched flag.
 *
 * @details
 * The intended NMI path: snapshots SRAMESR once, walks each of the 8
 * defined error bits, calls ``ra8_sram_dispatch`` for every set bit
 * with the corresponding EAR address, and returns the OR of all
 * dispatched flags so the caller can clear them in a single
 * SRAMESCLR write.
 *
 * @param[out] out_status  Optional snapshot of the SRAMESR / EAR state
 *                         (may be NULL if the caller does not need it).
 * @return Bitmask of flags that were dispatched (in SRAMESR encoding).
 *
 * @pre  ``ra8_sram_init`` has run.
 * @pre  Caller is willing to clear the returned mask via
 *       ``ra8_sram_clear_status``.
 * @post Every set bit in SRAMESR fires a callback exactly once.
 * @post No register state is mutated by this call -- clearing is the
 *       caller's responsibility (so per-bank handlers can decide
 *       whether to consume the address before clearing).
 *
 * @note Safe to call from NMI context.
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 */
uint16_t ra8_sram_dispatch_from_esr(ra8_sram_status_t* out_status);

#ifdef __cplusplus
}
#endif
