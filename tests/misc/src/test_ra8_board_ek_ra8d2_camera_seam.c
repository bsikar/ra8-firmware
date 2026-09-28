/**
 * @file test_ra8_board_ek_ra8d2_camera_seam.c
 * @brief Host vectors for the EK-RA8D2 camera bus published as a house seam.
 *
 * @details
 * `ra8_board_camera_i2c_ops` is the board's answer to four copies of one SCCB
 * adapter: rather than every camera app packing its own 16-bit register
 * pointer, the board publishes RIIC1 through ::ra8_i2c_bus_ops_t and a sensor
 * binder consumes it. These vectors pin the two things a caller depends on,
 * namely that a null destination is refused and that a successful call leaves
 * no callback empty, because a half-filled seam would fail later inside the
 * sensor rather than here.
 *
 * The transfer semantics themselves belong to the RIIC backend and are
 * covered by `test_ra8_io_i2c_bus.c`; this file only proves the publication.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "unity_minimal.h"

/**
 * @brief The publisher refuses a null destination.
 * @details Its only refusal case, and the one a caller can trip.
 * @pre None.
 * @post No bus handle was bound.
 * @note Test-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_rejects_null(void)
{
  TEST_BEGIN("board.camera: i2c_ops rejects a null destination");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_camera_i2c_ops(nullptr));
  TEST_END("board.camera: i2c_ops rejects a null destination");
}

/**
 * @brief A successful publication leaves no callback empty.
 * @details A seam missing `transfer` binds fine and then fails on the first
 *          register read, so the completeness check belongs here.
 * @pre None.
 * @post The board camera bus handle is bound.
 * @note Test-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_is_complete(void)
{
  TEST_BEGIN("board.camera: i2c_ops publishes a complete seam");
  ra8_i2c_bus_ops_t ops = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_i2c_ops(&ops));
  TEST_ASSERT_NOT_NULL(ops.write);
  TEST_ASSERT_NOT_NULL(ops.read);
  TEST_ASSERT_NOT_NULL(ops.transfer);
  TEST_ASSERT_NOT_NULL(ops.ctx);
  TEST_END("board.camera: i2c_ops publishes a complete seam");
}

/**
 * @brief Republishing is idempotent and keeps one board-owned handle.
 * @details Two apps in one image may each ask for the seam; both must get the
 *          same context rather than a second bus.
 * @pre None.
 * @post The board camera bus handle is bound.
 * @note Test-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_is_stable(void)
{
  TEST_BEGIN("board.camera: i2c_ops republishes the same bus");
  ra8_i2c_bus_ops_t first  = {};
  ra8_i2c_bus_ops_t second = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_i2c_ops(&first));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_i2c_ops(&second));
  TEST_ASSERT_EQ(first.ctx, second.ctx);
  TEST_ASSERT_EQ(first.transfer, second.transfer);
  TEST_END("board.camera: i2c_ops republishes the same bus");
}

int main(void)
{
  internal_test_seam_rejects_null();
  internal_test_seam_is_complete();
  internal_test_seam_is_stable();
  return 0;
}
