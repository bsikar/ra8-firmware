/**
 * @file fuzz_ra8_usb_pal.c
 * @brief libFuzzer harness for the USB PAL endpoint open / send / recv path.
 *
 * @details
 * USB descriptors and bulk OUT payloads originate at the host (or
 * across a USB-IF compliance test stand) and reach the device as
 * arbitrary bytes. The PAL must validate every endpoint address and
 * length without crashing or running past the per-EP ring buffers.
 *
 * The harness brings up the PAL on the FS controller once, then on
 * each iteration consumes the libFuzzer input as:
 *
 *   byte[0]   raw "endpoint descriptor" -- bits[6:0] = ep_addr, bit[7] = direction
 *   byte[1]   ep_type    (control / iso / bulk / intr; modulo 4)
 *   byte[2-3] max_packet (LE16)
 *   byte[4..] payload    (sent on IN, then drained on OUT regardless of dir)
 *
 * Each iteration calls ra8_usb_pal_ep_open() with the parsed values,
 * then ra8_usb_pal_ep_send() with the payload, then attempts a recv
 * to drain whatever the PAL routed back. Errors are silently ignored
 * -- the goal is to exercise the validators, not to assert success.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_pal.h"

enum : uint16_t {
  k_fuzz_min_input    = 5U,    /**< Fuzz minimum input.   */
  k_fuzz_max_input    = 1024U, /**< Fuzz maximum input.   */
  k_fuzz_recv_cap     = 1024U, /**< Fuzz recv cap.        */
  k_fuzz_hdr_bytes    = 4U,    /**< Fuzz hdr bytes.       */
  k_fuzz_ep_dir_bit   = 0x80U, /**< Fuzz ep dir bit.      */
  k_fuzz_ep_addr_mask = 0x7FU, /**< Fuzz ep address mask. */
  k_fuzz_ep_type_mod  = 4U,    /**< Fuzz ep type mod.     */
};

static uint8_t s_initialized;
static uint8_t s_recv_buf[k_fuzz_recv_cap];

static int fuzz_pal_setup(void)
{
  (void)ra8_mstp_init();
  (void)ra8_usb_pal_deinit();
  if (ra8_usb_pal_init(k_ra8_usb_speed_fs) != k_ra8_ok) {
    return -1;
  }
  if (ra8_usb_pal_attach(true) != k_ra8_ok) {
    return -1;
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size < (size_t)k_fuzz_min_input || size > (size_t)k_fuzz_max_input) {
    return 0;
  }
  if (s_initialized == 0U) {
    if (fuzz_pal_setup() != 0) {
      return 0;
    }
    s_initialized = 1U;
  }

  const uint8_t              raw_desc = data[0];
  const uint8_t              ep_addr  = (uint8_t)(raw_desc & (uint8_t)k_fuzz_ep_addr_mask);
  const uint8_t              dir_bit  = (uint8_t)(raw_desc & (uint8_t)k_fuzz_ep_dir_bit);
  const ra8_usb_pal_ep_dir_t dir =
    (dir_bit != 0U) ? k_ra8_usb_pal_ep_dir_in : k_ra8_usb_pal_ep_dir_out;
  const ra8_usb_pal_ep_type_t type = (ra8_usb_pal_ep_type_t)(data[1] % (uint8_t)k_fuzz_ep_type_mod);
  const uint16_t max_packet        = (uint16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8U));

  (void)ra8_usb_pal_ep_open(ep_addr, dir, type, max_packet);

  const uint8_t* payload     = &data[k_fuzz_hdr_bytes];
  const uint16_t payload_len = (uint16_t)(size - (size_t)k_fuzz_hdr_bytes);
  (void)ra8_usb_pal_ep_send(ep_addr, payload, payload_len);

  uint16_t cap = (uint16_t)sizeof s_recv_buf;
  (void)ra8_usb_pal_ep_recv(ep_addr, s_recv_buf, &cap);
  return 0;
}
