/**
 * @file ra8_sim_xspi_flash.h
 * @brief Host-test register-level JEDEC NOR-flash model behind the xSPI engine
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S} (host test-only)
 *
 * @details
 * Models the xSPI manual-command engine plus the on-board IS25LX512M NOR
 * flash at REGISTER level for the host unit-test build, mirroring the
 * board_sim peripheral model in ``tools/board_sim/src/periph/board_periph_xspi.c``.
 * It replaces the deleted in-driver ``RA8_SIMULATOR_MODE`` fake-flash
 * short-circuits (#238): with the model installed, ``ra8_xspi_flash.c``
 * runs its real "fill CDBUF, set CDCTL0.TRREQ, poll INTS.CMDCMP, read
 * CDBUF" MMIO sequence on the host and the data genuinely round-trips.
 *
 * The model is driven synchronously from the driver's own CMDCMP poll via
 * the ::ra8_sim_mmio_set_poll_hook seam: on every poll it services any
 * pending ``TRREQ`` kick on either xSPI instance -- decode the CDBUF
 * slot-0 descriptor, execute the JEDEC opcode against a per-instance NOR
 * backing array, raise ``INTS.CMDCMP``, and self-clear ``TRREQ`` -- and it
 * applies the ``INTC`` write-1-to-clear semantics against ``INTS``.
 *
 * Decoded opcodes (all others complete with no flash effect):
 * - ``0x9F`` RDID -> CDD0 = {0x9D, 0x5A, 0x1A} (IS25LX512M JEDEC triplet)
 * - ``0x05`` RDSR -> CDD0 = WEL=1 plus WIP=1 while a busy budget remains
 * - ``0x06`` WREN -> no-op (latch is implicit; RDSR always reports WEL=1)
 * - ``0x03`` READ -> copy DATASIZE bytes from the backing array into CDD0/CDD1
 * - ``0x02`` PP   -> AND DATASIZE bytes of CDD0/CDD1 into the backing array
 * - ``0x20`` SE   -> restore the 4 KiB sector at CDA to 0xFF
 *
 * NOR semantics (program only clears bits; erase restores 0xFF) match the
 * real part and the board_sim model. A test drives the driver's WIP-poll
 * retry / timeout legs with ::ra8_sim_xspi_flash_set_busy_polls; the CMDCMP
 * timeout leg is driven by arming ``ra8_sim_mmio_fail_wait(&reg->INTS)``,
 * which overrides the poll-loop exit even though the hook still services
 * the command.
 *
 * Usage from a test:
 * @code
 * static void prep(void)
 * {
 *   ra8_sim_mmap_reset();
 *   ra8_sim_mmio_reset();   // clears any poll hook ...
 *   ra8_sim_xspi_flash_install(); // ... so install the model afterwards
 * }
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_sim_xspi_flash_vals_t
 * @brief Fixed constants of the host NOR-flash model.
 *
 * @details
 * The backing array spans the full 3-byte JEDEC address space the driver
 * can emit (16 MiB), so every address that passes the driver's range
 * validation is genuinely backed. The JEDEC identity matches the on-board
 * EK-RA8D2 IS25LX512M and the board_sim model, packed in the
 * ``(manufacturer << 16) | (type << 8) | capacity`` order that
 * ``ra8_xspi_flash_read_id()`` returns.
 *
 * @invariant ``k_ra8_sim_xspi_flash_bytes`` equals the 3-byte address space.
 *
 * @see ra8_sim_xspi_flash_install() Model installation.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_sim_xspi_flash_bytes        = 0x1000000UL, /**< 16 MiB: full 3-byte space. */
  k_ra8_sim_xspi_flash_jedec_id     = 0x9D5A1AUL,  /**< Packed IS25LX512M RDID.    */
  k_ra8_sim_xspi_flash_status_idle  = 0x02U,       /**< RDSR when idle: WEL=1.     */
  k_ra8_sim_xspi_flash_busy_forever = 0xFFFFFFFFU, /**< Busy budget that outlasts
                                                    *   every driver WIP poll.     */
} ra8_sim_xspi_flash_vals_t;

/**
 * @brief Install the register-level NOR model and wipe it to erased state.
 *
 * @details
 * Registers the model's servicing hook via ::ra8_sim_mmio_set_poll_hook and
 * resets all model state: both instances' backing arrays read back 0xFF
 * (erased NOR) and every busy budget is zero (program/erase complete on the
 * first RDSR poll). Call from a test's prep helper AFTER
 * ::ra8_sim_mmio_reset, which clears any installed poll hook.
 *
 * @pre Called from a host (``RA8_SIMULATOR_MODE`` + ``UNIT_TEST``) test binary.
 * @pre ::ra8_sim_mmio_reset ran first in this test case (hook slot is free).
 * @post The model hook is installed; TRREQ kicks are serviced on every poll.
 * @post Both instances' flash arrays are fully erased (0xFF) with WIP idle.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @see ra8_sim_xspi_flash_set_busy_polls() Drive the WIP retry/timeout legs.
 * @since 0.1.0
 */
void ra8_sim_xspi_flash_install(void);

/**
 * @brief Report WIP busy for @p n RDSR polls after each program / erase.
 *
 * @details
 * Models a slow page-program or sector-erase: after every subsequent PP or
 * SE command the model answers the next @p n RDSR commands with WIP=1
 * before reporting idle, so the driver's bounded WIP-clear loop runs its
 * continuation branch @p n times. Pass
 * ::k_ra8_sim_xspi_flash_busy_forever to outlast the driver's whole poll
 * budget and drive its timeout leg. ::ra8_sim_xspi_flash_install resets the
 * budget to zero (instant completion).
 *
 * @param[in] instance xSPI instance index (0 or 1).
 * @param[in] n        RDSR polls that report busy after each PP / SE.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Busy budget recorded for @p instance.
 * @retval k_ra8_err_invalid_arg @p instance is out of range.
 *
 * @pre ::ra8_sim_xspi_flash_install ran in this test case.
 * @pre @p instance is below ``k_ra8_xspi_instance_count``.
 * @post The next PP / SE arms an @p n-poll busy window on @p instance.
 * @post No flash content is modified.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @see ra8_sim_xspi_flash_install() Reset the budget to zero.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sim_xspi_flash_set_busy_polls(uint8_t instance, uint32_t n);

#ifdef __cplusplus
}
#endif
