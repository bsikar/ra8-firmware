/**
 * @file usb_selftest_host.h
 * @brief Host-side entry point of the soak self-test
 *
 * @details
 * One full host pass over the loop cable: enumerate the device, mount its FAT16
 * volume, repeat the 1 MiB integrity sweep ::k_selftest_soak_iters times, confirm
 * the RO write rejection, and print the aggregate throughput + SOAK PASS banner.
 * Driven by the host worker thread in main.c. Defined in usb_selftest_host.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include "ra8_err.h"

/**
 * @brief Run one full soak pass and print the verdict.
 *
 * @return k_ra8_ok if every iteration matched and SOAK PASS was printed;
 *         otherwise the first failing step's error (host controller closed).
 *
 * @pre The device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success LED2 is on and the aggregate + PASS lines are queued.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_host_pass(void);
