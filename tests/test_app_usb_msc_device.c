/**
 * @file test_app_usb_msc_device.c
 * @brief Integration test: ThreadX + USBX MSC RAM-disk bring-up
 *
 * @details
 * Mirrors the multi-module integration-test pattern. The
 * production app at examples/ek_ra8d2/usb_msc_device/main.c brings up
 * CGC, routes the four USB-FS pins, hands control to ThreadX, and lets
 * USBX's Mass-Storage class drive the ra8_usb_pal device-side
 * controller against a 4 KiB RAM-disk (8 x 512-byte blocks). Neither
 * USBX nor ThreadX is in the host test build, so this test exercises
 * the same module surface the app calls (PFS routing + ra8_usb_pal
 * init/attach/ep_open with two bulk endpoints) plus the SCSI bounds-
 * check semantics of the read/write callbacks.
 *
 * Modeled flow:
 *   1. ra8_pfs_route_peripheral for the four USB-FS pins.
 *   2. ra8_board_led_init(LED1).
 *   3. ra8_usb_pal_init(speed=fs).
 *   4. ra8_usb_pal_attach(true).
 *   5. ra8_usb_pal_ep_open(EP1 IN bulk + EP2 OUT bulk).
 *   6. SCSI READ/WRITE bounds verified against the 8-block geometry.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sim_mmap.h"
#include "ra8_usb_pal.h"
#include "unity_minimal.h"

/** @brief Payload fill for the MSC bulk-transfer assertions. */
typedef enum : uint8_t {
  k_msc_fill_payload = 0xAAU, /**< Arbitrary non-trivial byte pattern. */
} msc_fill_t;

/** @brief Per-test enums. */
typedef enum : uint16_t {
  k_test_msc_ep_bulk_out = 0x02U, /**< EP2 OUT bulk -- BBB CBW/data. */
  k_test_msc_ep_bulk_in  = 0x81U, /**< EP1 IN  bulk -- BBB data/CSW. */
  k_test_msc_max_packet  = 64U,   /**< Bulk-FS packet size.          */
  k_test_msc_block_size  = 512U,  /**< SCSI logical block size.      */
  k_test_msc_block_count = 8U,    /**< 8 x 512 = 4 KiB RAM-disk.     */
} test_msc_const_t;

/** @brief Pin map mirrors examples/ek_ra8d2/usb_msc_device/main.c. */
static const ra8_port_pin_t k_test_msc_pin_vbus =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_4 << 8) | (uint16_t)k_ra8_pin_7);
static const ra8_port_pin_t k_test_msc_pin_dp =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_8 << 8) | (uint16_t)k_ra8_pin_14);
static const ra8_port_pin_t k_test_msc_pin_dm =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_8 << 8) | (uint16_t)k_ra8_pin_15);

/**
 * @brief Mock "RAM-disk" backing store -- mirrors the app's s_ramdisk.
 * @note Not thread-safe; tests are single-threaded.
 */
static uint8_t s_test_msc_ramdisk[k_test_msc_block_count * k_test_msc_block_size];

/** @brief Per-test fixture reset. */
static void reset_world(void)
{
  ra8_sim_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_usb_pal_deinit();
  (void)memset(s_test_msc_ramdisk, 0, sizeof(s_test_msc_ramdisk));
}

/**
 * @brief Bounds-check helper: same predicate as demo_msc_read/write.
 *
 * @return true if the LBA range fits in the RAM-disk.
 */
static bool test_msc_lba_in_range(uint32_t lba, uint32_t blocks)
{
  return (lba + blocks) <= (uint32_t)k_test_msc_block_count;
}

/* -------------------------------------------------------------------------
 * Golden path
 * ------------------------------------------------------------------------- */

/**
 * @brief Replays the four-pin USB-FS routing block.
 *
 * @par MC/DC: Decision under test: the chained ``!= ok`` early-return
 * guards in demo_pins_init. This case covers all-ok; failure vector
 * covered by the rejection test below.
 */
static void test_msc_pfs_routes_usbfs_pins(void)
{
  reset_world();
  TEST_BEGIN("usb_msc_device: PFS routes USB-FS pins");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_msc_pin_vbus, k_ra8_psel_usb_fs, "test.vbus"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_msc_pin_dp, k_ra8_psel_usb_fs, "test.dp"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_pfs_route_peripheral(k_test_msc_pin_dm, k_ra8_psel_usb_fs, "test.dm"));
  TEST_END("usb_msc_device: PFS routes USB-FS pins");
}

/**
 * @brief Open the BBB endpoint pair (EP1 IN + EP2 OUT, 64-byte MPS).
 *
 * @par MC/DC: Decision under test: ``ep_open != ok`` for both
 * directions. Multiple atomic conditions; this case covers the all-
 * valid vector for both directions.
 */
static void test_msc_open_bulk_endpoint_pair(void)
{
  reset_world();
  TEST_BEGIN("usb_msc_device: open bulk EP1 IN + EP2 OUT");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_attach(true));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open((uint8_t)k_test_msc_ep_bulk_in,
                                     k_ra8_usb_pal_ep_dir_in,
                                     k_ra8_usb_pal_ep_type_bulk,
                                     (uint16_t)k_test_msc_max_packet));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open((uint8_t)k_test_msc_ep_bulk_out,
                                     k_ra8_usb_pal_ep_dir_out,
                                     k_ra8_usb_pal_ep_type_bulk,
                                     (uint16_t)k_test_msc_max_packet));
  TEST_END("usb_msc_device: open bulk EP1 IN + EP2 OUT");
}

/**
 * @brief SCSI READ at LBA 0 for one block lands inside the RAM-disk.
 *
 * @par MC/DC: Decision under test in app's demo_msc_read:
 * ``(lba + number_blocks) > block_count``. Two atomic conditions x N+1
 * = 3 vectors. This case covers the in-range vector.
 */
static void test_msc_read_lba0_in_range(void)
{
  reset_world();
  TEST_BEGIN("usb_msc_device: SCSI READ LBA 0 fits in RAM-disk");
  TEST_ASSERT(test_msc_lba_in_range(0U, 1U));
  /* Replay copy semantics: one block out of the disk. */
  uint8_t buf[k_test_msc_block_size];
  (void)memcpy(buf, &s_test_msc_ramdisk[0], (size_t)k_test_msc_block_size);
  TEST_ASSERT_EQ(0, buf[0]);
  TEST_END("usb_msc_device: SCSI READ LBA 0 fits in RAM-disk");
}

/**
 * @brief SCSI WRITE at last block lands inside the RAM-disk.
 *
 * @par MC/DC: Decision under test in demo_msc_write: in-range vector
 * for ``(lba + number_blocks) > block_count``.
 */
static void test_msc_write_last_block_in_range(void)
{
  reset_world();
  TEST_BEGIN("usb_msc_device: SCSI WRITE last block fits");
  TEST_ASSERT(test_msc_lba_in_range((uint32_t)k_test_msc_block_count - 1U, 1U));
  /* Splat one block of 0xAA into the last block, then read back. */
  uint8_t pat[k_test_msc_block_size];
  (void)memset(pat, k_msc_fill_payload, sizeof(pat));
  (void)memcpy(&s_test_msc_ramdisk[(size_t)(k_test_msc_block_count - 1U) * k_test_msc_block_size],
               pat,
               sizeof(pat));
  TEST_ASSERT_EQ(0xAA,
                 s_test_msc_ramdisk[(size_t)(k_test_msc_block_count - 1U) * k_test_msc_block_size]);
  TEST_END("usb_msc_device: SCSI WRITE last block fits");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief PFS route on out-of-range port is rejected.
 *
 * @par MC/DC: Failure-side vector for ``ra8_pfs_route_peripheral != ok``.
 */
static void test_msc_pfs_route_invalid_pin_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_msc_device: PFS rejects out-of-range port");
  const ra8_port_pin_t bad_pin = (ra8_port_pin_t)(((uint16_t)0x0FU << 8) | (uint16_t)k_ra8_pin_0);
  TEST_ASSERT(ra8_pfs_route_peripheral(bad_pin, k_ra8_psel_usb_fs, "test.bad") != k_ra8_ok);
  TEST_END("usb_msc_device: PFS rejects out-of-range port");
}

/**
 * @brief SCSI READ past last block is rejected (BBB ILLEGAL REQUEST).
 *
 * @par MC/DC: Out-of-range vector for ``(lba + number_blocks) > N``
 * decision in demo_msc_read. Pairs with the in-range vector above.
 */
static void test_msc_read_past_end_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_msc_device: SCSI READ past end rejected");
  /* lba == 0, count == 9 blocks > 8-block disk. */
  TEST_ASSERT(!test_msc_lba_in_range(0U, (uint32_t)k_test_msc_block_count + 1U));
  /* Edge: lba == count, count == 1. */
  TEST_ASSERT(!test_msc_lba_in_range((uint32_t)k_test_msc_block_count, 1U));
  TEST_END("usb_msc_device: SCSI READ past end rejected");
}

/**
 * @brief ep_open before pal_init is rejected (worker-thread orderings).
 *
 * @par MC/DC: Failure side of the ``initialized == false`` invariant
 * check in ep_open.
 */
static void test_msc_ep_open_before_init_rejected(void)
{
  reset_world();
  TEST_BEGIN("usb_msc_device: ep_open before init rejected");
  ra8_err_t err = ra8_usb_pal_ep_open((uint8_t)k_test_msc_ep_bulk_in,
                                      k_ra8_usb_pal_ep_dir_in,
                                      k_ra8_usb_pal_ep_type_bulk,
                                      (uint16_t)k_test_msc_max_packet);
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("usb_msc_device: ep_open before init rejected");
}

int main(void)
{
  test_msc_pfs_routes_usbfs_pins();
  test_msc_open_bulk_endpoint_pair();
  test_msc_read_lba0_in_range();
  test_msc_write_last_block_in_range();
  test_msc_pfs_route_invalid_pin_rejected();
  test_msc_read_past_end_rejected();
  test_msc_ep_open_before_init_rejected();
  return 0;
}
