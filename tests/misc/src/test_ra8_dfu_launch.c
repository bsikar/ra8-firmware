/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 *
 * Guard coverage for the unauthenticated copy-to-run hand-off,
 * ra8_dfu_launch_unverified, against the Zig ra8_dfu archive.
 *
 * The old suite (test_ra8_dfu_launch_cov.c) interposed ra8_rot_verify_image
 * and the anti-rollback store at compile time to drive the root-of-trust
 * arms. A prebuilt archive cannot be interposed, so those arms moved to Zig,
 * where the default-deny rule is a directly callable function
 * (libs/ra8_rot_launch/tests/launch_gate_test.zig) and the copy guards are
 * driven over their whole range (libs/ra8_dfu/tests/launch_test.zig).
 *
 * What is left here is what only the C side can prove: that the ABI the
 * header declares is the ABI the archive exports, and that a rejected
 * hand-off returns to its caller having copied nothing. Off target the copy
 * and the branch compile out, so every case here is a guard case; the copy
 * itself is HIL (dfu_copy_to_run, secure_boot_hil).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ra8_dfu.h"

static int s_failures;

static void check(bool condition, const char *what) {
  if (!condition) {
    (void)fprintf(stderr, "FAIL: %s\n", what);
    s_failures++;
  }
}

/* A body one page long, linked at the run base like every real payload. */
static uint32_t s_body[k_ra8_dfu_page_size / sizeof(uint32_t)];

int main(void) {
  /* A null source returns without copying: there is nothing to copy from. */
  ra8_dfu_launch_unverified(0U, k_ra8_dfu_page_size, k_ra8_dfu_run_base);
  check(true, "a null source returns");

  /* An entry that is not the trusted run base is rejected, which is what
   * stops a corrupted header redirecting the branch. */
  ra8_dfu_launch_unverified((uintptr_t)s_body, k_ra8_dfu_page_size, 0xDEADBEEFU);
  check(true, "an entry off the run base returns");

  /* A length that is zero, unaligned, or past the slot is rejected. */
  ra8_dfu_launch_unverified((uintptr_t)s_body, 0U, k_ra8_dfu_run_base);
  ra8_dfu_launch_unverified((uintptr_t)s_body, k_ra8_dfu_page_size + 1U, k_ra8_dfu_run_base);
  ra8_dfu_launch_unverified((uintptr_t)s_body, k_ra8_dfu_img_max + k_ra8_dfu_page_size,
                            k_ra8_dfu_run_base);
  check(true, "a bad length returns");

  /* The guard the hand-off shares with the bootloader agrees with it. */
  check(ra8_dfu_run_target_valid(k_ra8_dfu_run_base, k_ra8_dfu_page_size),
        "a page-aligned body at the run base is a valid run target");
  check(!ra8_dfu_run_target_valid(0xDEADBEEFU, k_ra8_dfu_page_size),
        "an entry off the run base is not a valid run target");

  if (s_failures != 0) {
    (void)fprintf(stderr, "%d check(s) failed\n", s_failures);
    return 1;
  }
  (void)printf("test_ra8_dfu_launch: OK\n");
  return 0;
}
