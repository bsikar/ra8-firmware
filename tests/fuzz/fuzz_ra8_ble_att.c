/**
 * @file fuzz_ra8_ble_att.c
 * @brief libFuzzer harness for the BLE ATT/L2CAP receive path.
 *
 * @details
 * The host-side ATT dispatcher consumes L2CAP B-frames lifted off the
 * controller's ACL pipe and walks the per-opcode handlers
 * (internal_handle_find_info / read_by_type / read / write). Bytes
 * arrive from a remote peer over the air and must be parsed without
 * crashing or corrupting the static attribute table.
 *
 * The harness mirrors tests/test_ra8_ble_att_dispatch.c: bring up the
 * host stack, register one Primary Service + one Read/Write/Notify
 * characteristic, drive the stack into the connected state via the
 * UNIT_TEST inject hook, then feed each libFuzzer payload as the ATT
 * payload of an L2CAP frame on CID 0x0004.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_ble.h"
#include "ra8_ble_host.h"
#include "ra8_err.h"

/* UNIT_TEST hooks declared in ra8_ble_host.h. */

enum : uint16_t {
  k_fuzz_conn_handle   = 0x0040U,               /**< Fuzz conn handle.          */
  k_fuzz_l2cap_cid_att = 0x0004U,               /**< Fuzz L2CAP cid ATT.        */
  k_fuzz_l2cap_hdr_len = 4U,                    /**< Fuzz L2CAP hdr length.     */
  k_fuzz_max_att_pdu   = 512U,                  /**< Fuzz maximum ATT pdu.      */
  k_fuzz_max_frame_len = (uint16_t)(4U + 512U), /**< Fuzz maximum frame length. */
  k_fuzz_value_buf_len = 16U,                   /**< Fuzz value buffer length.  */
  k_fuzz_uuid_marker_s = 0xA0U,                 /**< Fuzz uuid marker s.        */
  k_fuzz_uuid_marker_c = 0xB0U,                 /**< Fuzz uuid marker c.        */
};

static uint8_t s_initialized;
static uint8_t s_value_buf[k_fuzz_value_buf_len];
static uint8_t s_frame[k_fuzz_max_frame_len];

static void fill_uuid(uint8_t* out, uint8_t marker)
{
  for (uint8_t i = 0U; i < 16U; i++) {
    out[i] = (uint8_t)(marker + i);
  }
}

static int fuzz_ble_setup(void)
{
  const ra8_ble_host_config_t cfg = {
    .role       = k_ra8_ble_host_role_peripheral,
    .name       = "fuzz",
    .appearance = 0x0040U,
  };
  if (ra8_ble_host_init(&cfg) != k_ra8_ok) {
    return -1;
  }
  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  fill_uuid(svc_uuid, (uint8_t)k_fuzz_uuid_marker_s);
  fill_uuid(chr_uuid, (uint8_t)k_fuzz_uuid_marker_c);

  uint16_t      svc = 0U;
  uint16_t      chr = 0U;
  const uint8_t props =
    (uint8_t)((uint8_t)k_ra8_ble_host_char_prop_read | (uint8_t)k_ra8_ble_host_char_prop_write |
              (uint8_t)k_ra8_ble_host_char_prop_notify);
  if (ra8_ble_host_gatt_register_service(svc_uuid, &svc) != k_ra8_ok) {
    return -1;
  }
  if (ra8_ble_host_gatt_register_char(svc,
                                      chr_uuid,
                                      props,
                                      s_value_buf,
                                      (uint16_t)sizeof s_value_buf,
                                      &chr) != k_ra8_ok) {
    return -1;
  }
  ra8_ble_host_test_inject_connect((uint16_t)k_fuzz_conn_handle);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_max_att_pdu) {
    return 0;
  }
  if (s_initialized == 0U) {
    if (fuzz_ble_setup() != 0) {
      return 0;
    }
    s_initialized = 1U;
  }
  /* L2CAP B-frame header: payload_len(LE16) + cid(LE16) + payload */
  const uint16_t att_len = (uint16_t)size;
  s_frame[0]             = (uint8_t)(att_len & 0xFFU);
  s_frame[1]             = (uint8_t)((att_len >> 8U) & 0xFFU);
  s_frame[2]             = (uint8_t)((uint16_t)k_fuzz_l2cap_cid_att & 0xFFU);
  s_frame[3]             = (uint8_t)(((uint16_t)k_fuzz_l2cap_cid_att >> 8U) & 0xFFU);
  (void)memcpy(&s_frame[k_fuzz_l2cap_hdr_len], data, size);
  ra8_ble_host_test_inject_acl((uint16_t)k_fuzz_conn_handle,
                               s_frame,
                               (uint16_t)((uint16_t)k_fuzz_l2cap_hdr_len + att_len));
  return 0;
}
