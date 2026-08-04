/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_usb_hmsc_enum_cov.c
 * @brief White-box line-coverage tests for the polled host-MSC enumeration
 *        ladder (`ra8_usb_hmsc_enum.c`).
 *
 * @details
 * The enumeration ladder in `ra8_usb_hmsc_enum.c` walks a device from attach
 * through address assignment, configuration parse, SET_CONFIGURATION,
 * GET_MAX_LUN and bulk-pipe setup. Every chapter-9 step rides
 * `ra8_usb_host_control_xfer`, whose success depends on real SIE handshakes
 * (SUREQ self-clear, the SACK/SIGN interrupt latch, BRDY/BEMP edges, CFIFO
 * fills). The host register mirror is plain RAM: the control engine clears
 * the very SACK bit it then spins on, and nothing re-asserts it, so a
 * transfer driven through the public `ra8_usb_hmsc_enumerate` can only ever
 * reach the timeout legs (already exercised by `test_ra8_usb_hmsc.c`).
 *
 * To reach the ladder's data-driven logic (descriptor parse, endpoint
 * selection, address assignment, publish) the module is compiled a second
 * time here as a private instrumented copy: this TU `#include`s the module
 * source with its hardware-transport dependencies redirected -- via
 * preprocessor rename -- to deterministic, test-scripted mocks. The one
 * exported symbol (`ra8_usb_hmsc_enumerate`) is renamed to
 * `ra8_usb_hmsc_enumerate_cov` so it does not collide with the production
 * copy linked from `ra8_core_hal`. The mocks feed realistic USB descriptor
 * bytes so the REAL production logic runs line-by-line; only the wire is
 * faked. No hardware line is bypassed by an exclusion marker.
 *
 * The shared shadow state `s_usb_hmsc_state` resolves to the single
 * production definition in `ra8_usb_hmsc.c` (linked from `ra8_core_hal`), so
 * the instrumented copy reads and writes the same object the production
 * driver does.
 */

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"
#include "unity_minimal.h"

/**
 * @enum t_enum_desc_t
 * @brief Descriptor offsets and standard requests the enumeration mock answers.
 *
 * @details
 * Offsets are relative to the start of an interface descriptor: byte 5 is
 * bInterfaceClass and byte 7 is bInterfaceProtocol. The mock recognises the
 * three requests below and stalls everything else.
 */
typedef enum : uint8_t {
  k_t_iface_off_class    = 5U,    /**< bInterfaceClass offset.             */
  k_t_iface_off_protocol = 7U,    /**< bInterfaceProtocol offset.          */
  k_t_req_set_address    = 0x05U, /**< Standard request SET_ADDRESS.       */
  k_t_req_set_config     = 0x09U, /**< Standard request SET_CONFIGURATION. */
  k_t_req_get_max_lun    = 0xFEU, /**< Class request GET_MAX_LUN.          */
  k_t_byte_mask          = 0xFFU, /**< Low-byte mask while splitting a 16-bit
                                       descriptor field; also the unassigned
                                       device address the hunt starts from.    */
} t_enum_desc_t;

/**
 * @enum t_enum_budget_t
 * @brief Blob capacity and the mock clock step that bounds the attach wait.
 */
typedef enum : uint16_t {
  k_t_cfg_blob_cap = 64U,   /**< Configuration-descriptor scratch, bytes. */
  k_t_time_step_us = 1500U, /**< Clock step per poll: 0 -> 1500 (still inside
                                 the 2000 us budget) -> 3000 (past it, so the
                                 wait breaks), giving both loop outcomes.       */
} t_enum_budget_t;

/**
 * @enum test_enum_cov_desc_t
 * @brief USB descriptor byte constants used to craft mock responses.
 *
 * @details Values come from USB 2.0 chapter 9 (descriptor types / device
 * fields) and the USB Mass-Storage-Class Bulk-Only Transport rev 1.0
 * (class 0x08 / subclass 0x06 / protocol 0x50). Named here so the crafted
 * descriptor blobs read as protocol layout, not magic bytes.
 */
typedef enum : uint16_t {
  k_tc_dev_desc_len   = 18U,     /**< DEVICE descriptor length.          */
  k_tc_cfg_hdr_len    = 9U,      /**< CONFIGURATION header length.       */
  k_tc_iface_desc_len = 9U,      /**< INTERFACE descriptor length.       */
  k_tc_ep_desc_len    = 7U,      /**< ENDPOINT descriptor length.        */
  k_tc_dtype_device   = 0x01U,   /**< bDescriptorType DEVICE.            */
  k_tc_dtype_config   = 0x02U,   /**< bDescriptorType CONFIGURATION.     */
  k_tc_dtype_iface    = 0x04U,   /**< bDescriptorType INTERFACE.         */
  k_tc_dtype_endpoint = 0x05U,   /**< bDescriptorType ENDPOINT.          */
  k_tc_class_msc      = 0x08U,   /**< bInterfaceClass mass storage.      */
  k_tc_subclass_scsi  = 0x06U,   /**< bInterfaceSubClass SCSI.           */
  k_tc_protocol_bbb   = 0x50U,   /**< bInterfaceProtocol Bulk-Only.      */
  k_tc_class_hid      = 0x03U,   /**< A non-MSC interface class.         */
  k_tc_sub_rbc        = 0x01U,   /**< A non-SCSI subclass (RBC).         */
  k_tc_proto_cbi      = 0x51U,   /**< A non-BOT protocol (CBI).          */
  k_tc_ep_in_addr     = 0x81U,   /**< bEndpointAddress: IN, endpoint 1.  */
  k_tc_ep_out_addr    = 0x02U,   /**< bEndpointAddress: OUT, endpoint 2. */
  k_tc_ep_in2_addr    = 0x83U,   /**< A second IN endpoint (num 3).      */
  k_tc_ep_out2_addr   = 0x04U,   /**< A second OUT endpoint (num 4).     */
  k_tc_attr_bulk      = 0x02U,   /**< bmAttributes bulk transfer.        */
  k_tc_attr_int       = 0x03U,   /**< bmAttributes interrupt transfer.   */
  k_tc_mps_lo         = 0x40U,   /**< wMaxPacketSize LSB (64).           */
  k_tc_mps            = 0x0040U, /**< wMaxPacketSize (64).               */
  k_tc_vid            = 0x1234U, /**< Fabricated idVendor.               */
  k_tc_pid            = 0xABCDU, /**< Fabricated idProduct.              */
  k_tc_off_vid        = 8U,      /**< idVendor LSB offset in DEVICE.     */
  k_tc_off_pid        = 10U,     /**< idProduct LSB offset in DEVICE.    */
  k_tc_off_total      = 2U,      /**< wTotalLength LSB in CONFIGURATION. */
  k_tc_off_cfgval     = 5U,      /**< bConfigurationValue in CONFIG.     */
  k_tc_cfg_value      = 1U,      /**< bConfigurationValue we advertise.  */
  k_tc_iface_number   = 0U,      /**< bInterfaceNumber we advertise.     */
  k_tc_clamp_total    = 200U,    /**< A wTotalLength above the 128 cap.  */
  k_tc_lun_val        = 3U,      /**< A GET_MAX_LUN payload value.       */
  k_tc_byte_bits      = 8U,      /**< Bit width of one byte.             */
} test_enum_cov_desc_t;

/* =============================================================================
 * Test-scripted mock backends for the enumeration ladder's dependencies.
 * =============================================================================
 */

/** @brief GET_DESCRIPTOR(DEVICE) payload the control mock returns. */
static uint8_t s_dev_desc[k_tc_dev_desc_len];
/** @brief GET_DESCRIPTOR(CONFIGURATION) payload the control mock returns. */
static uint8_t s_cfg_blob[k_t_cfg_blob_cap];
/** @brief Valid byte count in ::s_cfg_blob. */
static uint16_t s_cfg_len;
/** @brief Bytes the control mock reports for a DEVICE descriptor read. */
static uint16_t s_dev_rx;
/** @brief Result the control mock returns for a DEVICE descriptor read. */
static ra8_err_t s_dev_err;
/** @brief Result the control mock returns for a CONFIGURATION read. */
static ra8_err_t s_cfg_err;
/** @brief Result the control mock returns for SET_ADDRESS. */
static ra8_err_t s_setaddr_err;
/** @brief Result the control mock returns for SET_CONFIGURATION. */
static ra8_err_t s_setcfg_err;
/** @brief Result the control mock returns for GET_MAX_LUN. */
static ra8_err_t s_lun_err;
/** @brief Bytes the control mock reports for a GET_MAX_LUN read. */
static uint16_t s_lun_rx;
/** @brief GET_MAX_LUN payload byte the control mock returns. */
static uint8_t s_lun_val;
/** @brief Value ::mock_line_state reports (non-zero => device attached). */
static uint16_t s_line_state;
/** @brief Result ::mock_pipe_setup returns. */
static ra8_err_t s_pipe_err;
/** @brief Next value ::mock_time_ms returns; advances by ::s_time_step. */
static uint32_t s_time_val;
/** @brief Increment applied to ::s_time_val after each ::mock_time_ms call. */
static uint32_t s_time_step;

/** @brief Attach-callback invocation count. */
static uint32_t s_attach_count;
/** @brief Context pointer captured by the attach callback. */
static void* s_attach_ctx_seen;
/** @brief Arbitrary context token handed to the attach callback. */
static const uintptr_t k_tc_ctx_token = 0xC0FFEE01U;

/**
 * @brief Serve a mocked GET_DESCRIPTOR request.
 *
 * @details
 * Device and configuration descriptors are served from separate fixture blobs
 * with independently injectable error codes and short-read counts, which is how
 * the enumeration coverage cases drive each failure branch in isolation. Any
 * other descriptor type succeeds with no data.
 *
 * @param[in]  setup        The SETUP packet under service.
 * @param[out] data         Buffer for the descriptor, or NULL.
 * @param[in]  data_len     Capacity of @p data in bytes.
 * @param[out] out_received Receives the byte count reported, or NULL.
 *
 * @return The injected result for the requested descriptor type.
 * @retval k_ra8_ok An unmodelled descriptor type was requested.
 *
 * @pre @p setup is non-NULL.
 * @pre The fixture blobs for the requested type are populated.
 * @post The copy is clamped to @p data_len, never overrunning @p data.
 * @post NULL @p data and @p out_received are both tolerated.
 *
 * @note Not thread-safe; the injection knobs are file-scope state.
 */
static ra8_err_t mock_get_descriptor(const ra8_usb_setup_t* setup,
                                     uint8_t*               data,
                                     uint16_t               data_len,
                                     uint16_t*              out_received)
{
  const uint8_t dtype = (uint8_t)((setup->w_value >> k_tc_byte_bits) & 0xFFU);
  if (dtype == (uint8_t)k_tc_dtype_device) {
    const uint16_t n =
      (data_len < (uint16_t)k_tc_dev_desc_len) ? data_len : (uint16_t)k_tc_dev_desc_len;
    if (data != nullptr) {
      (void)memcpy(data, s_dev_desc, (size_t)n);
    }
    if (out_received != nullptr) {
      *out_received = s_dev_rx;
    }
    return s_dev_err;
  }
  if (dtype == (uint8_t)k_tc_dtype_config) {
    const uint16_t n = (data_len < s_cfg_len) ? data_len : s_cfg_len;
    if (data != nullptr) {
      (void)memcpy(data, s_cfg_blob, (size_t)n);
    }
    if (out_received != nullptr) {
      *out_received = n;
    }
    return s_cfg_err;
  }
  return k_ra8_ok;
}

static ra8_err_t mock_ctrl_xfer(ra8_usb_speed_t        speed,
                                const ra8_usb_setup_t* setup,
                                uint8_t*               data,
                                uint16_t               data_len,
                                uint16_t*              out_received)
{
  (void)speed;
  const uint8_t req = setup->b_request;
  if (req == 0x06U) { /* GET_DESCRIPTOR */
    return mock_get_descriptor(setup, data, data_len, out_received);
  }
  if (req == k_t_req_set_address) { /* SET_ADDRESS */
    return s_setaddr_err;
  }
  if (req == k_t_req_set_config) { /* SET_CONFIGURATION */
    return s_setcfg_err;
  }
  if (req == k_t_req_get_max_lun) { /* GET_MAX_LUN */
    if ((data != nullptr) && (data_len >= 1U)) {
      data[0] = s_lun_val;
    }
    if (out_received != nullptr) {
      *out_received = s_lun_rx;
    }
    return s_lun_err;
  }
  return k_ra8_ok;
}

static uint16_t mock_line_state(ra8_usb_speed_t speed)
{
  (void)speed;
  return s_line_state;
}

static ra8_err_t mock_bus_reset(ra8_usb_speed_t speed, bool assert_reset)
{
  (void)speed;
  (void)assert_reset;
  return k_ra8_ok;
}

static ra8_err_t mock_set_uact(ra8_usb_speed_t speed, bool enable)
{
  (void)speed;
  (void)enable;
  return k_ra8_ok;
}

static ra8_err_t mock_set_target(ra8_usb_speed_t speed, uint8_t dev_addr)
{
  (void)speed;
  (void)dev_addr;
  return k_ra8_ok;
}

static ra8_err_t mock_pipe_setup(ra8_usb_speed_t speed,
                                 uint8_t         pipe_num,
                                 uint8_t         dev_addr,
                                 uint8_t         ep_num,
                                 bool            device_to_host,
                                 uint16_t        max_packet)
{
  (void)speed;
  (void)pipe_num;
  (void)dev_addr;
  (void)ep_num;
  (void)device_to_host;
  (void)max_packet;
  return s_pipe_err;
}

static void mock_delay_ms(uint32_t ms)
{
  (void)ms;
}

static uint32_t mock_time_ms(void)
{
  const uint32_t v = s_time_val;
  s_time_val       = s_time_val + s_time_step;
  return v;
}

static void cov_on_attach(void* ctx, const ra8_usb_hmsc_device_t* device)
{
  (void)device;
  ++s_attach_count;
  s_attach_ctx_seen = ctx;
}

/**
 * @brief Lay out a canonical MSC configuration blob into @p buf.
 *
 * @details CONFIGURATION header + one MSC SCSI Bulk-Only interface + a bulk
 * IN endpoint (num 1) + a bulk OUT endpoint (num 2). Returns the total byte
 * count and stamps wTotalLength.
 *
 * @param[out] buf Destination (>= 32 bytes).
 * @return Total descriptor-set byte count.
 */
static uint16_t build_msc_config(uint8_t* buf)
{
  uint16_t o = 0U;
  /* CONFIGURATION header. */
  buf[o + 0U]                     = (uint8_t)k_tc_cfg_hdr_len;
  buf[o + 1U]                     = (uint8_t)k_tc_dtype_config;
  buf[o + 2U]                     = 0U; /* wTotalLength LSB (patched below). */
  buf[o + 3U]                     = 0U; /* wTotalLength MSB.                 */
  buf[o + 4U]                     = 1U; /* bNumInterfaces.                   */
  buf[o + k_t_iface_off_class]    = (uint8_t)k_tc_cfg_value;
  buf[o + 6U]                     = 0U;
  buf[o + k_t_iface_off_protocol] = 0U;
  buf[o + 8U]                     = 0U;
  o                               = (uint16_t)(o + k_tc_cfg_hdr_len);
  /* INTERFACE: MSC SCSI Bulk-Only. */
  buf[o + 0U]                     = (uint8_t)k_tc_iface_desc_len;
  buf[o + 1U]                     = (uint8_t)k_tc_dtype_iface;
  buf[o + 2U]                     = (uint8_t)k_tc_iface_number;
  buf[o + 3U]                     = 0U;
  buf[o + 4U]                     = 2U; /* bNumEndpoints. */
  buf[o + k_t_iface_off_class]    = (uint8_t)k_tc_class_msc;
  buf[o + 6U]                     = (uint8_t)k_tc_subclass_scsi;
  buf[o + k_t_iface_off_protocol] = (uint8_t)k_tc_protocol_bbb;
  buf[o + 8U]                     = 0U;
  o                               = (uint16_t)(o + k_tc_iface_desc_len);
  /* ENDPOINT: bulk IN. */
  buf[o + 0U]                  = (uint8_t)k_tc_ep_desc_len;
  buf[o + 1U]                  = (uint8_t)k_tc_dtype_endpoint;
  buf[o + 2U]                  = (uint8_t)k_tc_ep_in_addr;
  buf[o + 3U]                  = (uint8_t)k_tc_attr_bulk;
  buf[o + 4U]                  = (uint8_t)k_tc_mps_lo;
  buf[o + k_t_iface_off_class] = 0U;
  buf[o + 6U]                  = 0U;
  o                            = (uint16_t)(o + k_tc_ep_desc_len);
  /* ENDPOINT: bulk OUT. */
  buf[o + 0U]                  = (uint8_t)k_tc_ep_desc_len;
  buf[o + 1U]                  = (uint8_t)k_tc_dtype_endpoint;
  buf[o + 2U]                  = (uint8_t)k_tc_ep_out_addr;
  buf[o + 3U]                  = (uint8_t)k_tc_attr_bulk;
  buf[o + 4U]                  = (uint8_t)k_tc_mps_lo;
  buf[o + k_t_iface_off_class] = 0U;
  buf[o + 6U]                  = 0U;
  o                            = (uint16_t)(o + k_tc_ep_desc_len);
  buf[k_tc_off_total]          = (uint8_t)(o & k_t_byte_mask);
  return o;
}

/** @brief Reset shadow state + mock scripts to a successful-enumeration baseline. */
static void reset_state(void)
{
  s_usb_hmsc_state.initialized = true;
  s_usb_hmsc_state.attached    = false;
  s_usb_hmsc_state.speed       = k_ra8_usb_speed_fs;
  s_usb_hmsc_state.attach_cb   = nullptr;
  s_usb_hmsc_state.attach_ctx  = nullptr;
  s_usb_hmsc_state.device      = (ra8_usb_hmsc_device_t){};

  (void)memset(s_dev_desc, 0, sizeof s_dev_desc);
  s_dev_desc[0]                 = (uint8_t)k_tc_dev_desc_len;
  s_dev_desc[1]                 = (uint8_t)k_tc_dtype_device;
  s_dev_desc[k_tc_off_vid]      = (uint8_t)((uint16_t)k_tc_vid & k_t_byte_mask);
  s_dev_desc[k_tc_off_vid + 1U] = (uint8_t)(((uint16_t)k_tc_vid >> k_tc_byte_bits) & k_t_byte_mask);
  s_dev_desc[k_tc_off_pid]      = (uint8_t)((uint16_t)k_tc_pid & k_t_byte_mask);
  s_dev_desc[k_tc_off_pid + 1U] = (uint8_t)(((uint16_t)k_tc_pid >> k_tc_byte_bits) & k_t_byte_mask);

  (void)memset(s_cfg_blob, 0, sizeof s_cfg_blob);
  s_cfg_len = build_msc_config(s_cfg_blob);

  s_dev_rx      = (uint16_t)k_tc_dev_desc_len;
  s_dev_err     = k_ra8_ok;
  s_cfg_err     = k_ra8_ok;
  s_setaddr_err = k_ra8_ok;
  s_setcfg_err  = k_ra8_ok;
  s_lun_err     = k_ra8_ok;
  s_lun_rx      = 1U;
  s_lun_val     = 0U;
  s_line_state  = 1U;
  s_pipe_err    = k_ra8_ok;
  s_time_val    = 0U;
  s_time_step   = 0U;

  s_attach_count    = 0U;
  s_attach_ctx_seen = nullptr;
}

/* Rename the one exported symbol so the instrumented copy does not clash with
 * the production `ra8_usb_hmsc_enumerate` linked from ra8_core_hal, and redirect
 * the hardware-transport calls to the deterministic mocks above. */
ra8_err_t ra8_usb_hmsc_enumerate_cov(ra8_usb_hmsc_device_t* out_device);

/** @brief RA8 USB hmsc enumerate. */
/* Include-time interposition seam: each macro below must be spelled
 * EXACTLY like the production symbol it replaces, so the project rule
 * that macro names are UPPER_CASE cannot apply here -- renaming one
 * silently un-hooks the mock and the test would exercise the real
 * driver while still passing. */
// NOLINTBEGIN(readability-identifier-naming)
#define ra8_usb_hmsc_enumerate ra8_usb_hmsc_enumerate_cov
/** @brief RA8 USB host control xfer. */
#define ra8_usb_host_control_xfer mock_ctrl_xfer
/** @brief RA8 USB host line state. */
#define ra8_usb_host_line_state mock_line_state
/** @brief RA8 USB host bus reset. */
#define ra8_usb_host_bus_reset mock_bus_reset
/** @brief RA8 USB host set uact. */
#define ra8_usb_host_set_uact mock_set_uact
/** @brief RA8 USB host set target. */
#define ra8_usb_host_set_target mock_set_target
/** @brief RA8 USB host pipe setup. */
#define ra8_usb_host_pipe_setup mock_pipe_setup
/** @brief RA8 delay ms. */
#define ra8_delay_ms mock_delay_ms
/** @brief RA8 time ms. */
#define ra8_time_ms mock_time_ms
// NOLINTEND(readability-identifier-naming)

#include "ra8_usb_hmsc_enum.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/* =============================================================================
 * White-box tests: each drives the real production helper on scripted bytes.
 * =============================================================================
 */

/**
 * @test test_read_dev_desc_short_and_full
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `internal_enum_read_dev_desc`
 * is a straight-line control read followed by a single-condition length check;
 * no `&&` or `||`.)
 */
static void test_read_dev_desc_short_and_full(void)
{
  TEST_BEGIN("read_dev_desc: full 18-byte read passes, short read fails");
  reset_state();

  uint8_t desc[k_tc_dev_desc_len] = {};
  /* Full read: 18 bytes -> k_ra8_ok. */
  s_dev_rx = (uint16_t)k_tc_dev_desc_len;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_read_dev_desc(desc));

  /* Short read: fewer than 18 bytes -> hw_error. */
  s_dev_rx = (uint16_t)(k_tc_dev_desc_len - 1U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_enum_read_dev_desc(desc));

  TEST_END("read_dev_desc: full 18-byte read passes, short read fails");
}

/**
 * @test test_hunt_success_and_attach_timeout
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the attach spin and the
 * (reset, address) hunt use single-condition guards only.)
 */
static void test_hunt_success_and_attach_timeout(void)
{
  TEST_BEGIN("hunt: device answers at addr 0; separate attach-timeout leg");

  /* Success leg: line state reports attach at once, addr-0 read answers. */
  reset_state();
  uint8_t desc[k_tc_dev_desc_len] = {};
  uint8_t addr                    = k_t_byte_mask;
  s_line_state                    = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_hunt(desc, &addr));
  TEST_ASSERT_EQ(0U, addr);

  /* Attach-timeout leg: line state never leaves SE0, so the spin exits on the
   * elapsed-milliseconds guard after two iterations; no device answers, so the
   * hunt exhausts every (reset, address) attempt and reports a timeout. */
  reset_state();
  s_line_state  = 0U;                   /* Never attaches.                            */
  s_time_val    = 0U;                   /* t0 = 0 on first read.                      */
  s_time_step   = k_t_time_step_us;     /* 0 -> 1500 (<=2000) -> 3000 (>2000, break). */
  s_dev_err     = k_ra8_err_hw_timeout; /* No descriptor ever answers.                */
  uint8_t addr2 = k_t_byte_mask;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, internal_enum_hunt(desc, &addr2));

  TEST_END("hunt: device answers at addr 0; separate attach-timeout leg");
}

/**
 * @test test_assign_addr_both_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- a single-condition
 * "already addressed?" guard gates the SET_ADDRESS path.)
 */
static void test_assign_addr_both_legs(void)
{
  TEST_BEGIN("assign_addr: skip when already addressed, else SET_ADDRESS to 1");
  reset_state();

  /* Already addressed (non-zero) -> early return, address unchanged. */
  uint8_t addr_nonzero = 2U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_assign_addr(&addr_nonzero));
  TEST_ASSERT_EQ(2U, addr_nonzero);

  /* Default address (0) -> SET_ADDRESS + retarget -> operating address 1. */
  uint8_t addr_zero = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_assign_addr(&addr_zero));
  TEST_ASSERT_EQ(1U, addr_zero);

  TEST_END("assign_addr: skip when already addressed, else SET_ADDRESS to 1");
}

/**
 * @test test_note_endpoint_all_slots
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- endpoint filtering and slot
 * selection are single-condition guards.)
 */
static void test_note_endpoint_all_slots(void)
{
  TEST_BEGIN("note_endpoint: records first bulk IN + OUT, ignores non-bulk/dups");
  reset_state();
  s_usb_hmsc_state.device = (ra8_usb_hmsc_device_t){};

  uint8_t ep_in[k_tc_ep_desc_len]   = {(uint8_t)k_tc_ep_desc_len,
                                       (uint8_t)k_tc_dtype_endpoint,
                                       (uint8_t)k_tc_ep_in_addr,
                                       (uint8_t)k_tc_attr_bulk,
                                       (uint8_t)k_tc_mps_lo,
                                       0U,
                                       0U};
  uint8_t ep_out[k_tc_ep_desc_len]  = {(uint8_t)k_tc_ep_desc_len,
                                       (uint8_t)k_tc_dtype_endpoint,
                                       (uint8_t)k_tc_ep_out_addr,
                                       (uint8_t)k_tc_attr_bulk,
                                       (uint8_t)k_tc_mps_lo,
                                       0U,
                                       0U};
  uint8_t ep_int[k_tc_ep_desc_len]  = {(uint8_t)k_tc_ep_desc_len,
                                       (uint8_t)k_tc_dtype_endpoint,
                                       (uint8_t)k_tc_ep_in2_addr,
                                       (uint8_t)k_tc_attr_int,
                                       (uint8_t)k_tc_mps_lo,
                                       0U,
                                       0U};
  uint8_t ep_in2[k_tc_ep_desc_len]  = {(uint8_t)k_tc_ep_desc_len,
                                       (uint8_t)k_tc_dtype_endpoint,
                                       (uint8_t)k_tc_ep_in2_addr,
                                       (uint8_t)k_tc_attr_bulk,
                                       (uint8_t)k_tc_mps_lo,
                                       0U,
                                       0U};
  uint8_t ep_out2[k_tc_ep_desc_len] = {(uint8_t)k_tc_ep_desc_len,
                                       (uint8_t)k_tc_dtype_endpoint,
                                       (uint8_t)k_tc_ep_out2_addr,
                                       (uint8_t)k_tc_attr_bulk,
                                       (uint8_t)k_tc_mps_lo,
                                       0U,
                                       0U};

  internal_enum_note_endpoint(ep_int); /* non-bulk -> ignored. */
  TEST_ASSERT_EQ(0U, s_usb_hmsc_state.device.bulk_in_ep);

  internal_enum_note_endpoint(ep_in); /* first bulk IN -> recorded. */
  TEST_ASSERT_EQ(1U, s_usb_hmsc_state.device.bulk_in_ep);
  TEST_ASSERT_EQ(k_tc_mps, s_usb_hmsc_state.device.bulk_in_max_packet);

  internal_enum_note_endpoint(ep_out); /* first bulk OUT -> recorded. */
  TEST_ASSERT_EQ(2U, s_usb_hmsc_state.device.bulk_out_ep);
  TEST_ASSERT_EQ(k_tc_mps, s_usb_hmsc_state.device.bulk_out_max_packet);

  internal_enum_note_endpoint(ep_in2);  /* IN slot filled -> unchanged.  */
  internal_enum_note_endpoint(ep_out2); /* OUT slot filled -> unchanged. */
  TEST_ASSERT_EQ(1U, s_usb_hmsc_state.device.bulk_in_ep);
  TEST_ASSERT_EQ(2U, s_usb_hmsc_state.device.bulk_out_ep);

  TEST_END("note_endpoint: records first bulk IN + OUT, ignores non-bulk/dups");
}

/**
 * @test test_iface_is_msc_all_fields
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the three class-field
 * checks are separate single-condition `if` statements, each with its own
 * early return, not a compound decision.)
 */
static void test_iface_is_msc_all_fields(void)
{
  TEST_BEGIN("iface_is_msc: true only for class 0x08 / sub 0x06 / proto 0x50");

  /* Interface descriptor: [5]=class [6]=subclass [7]=protocol. */
  uint8_t iface[k_tc_iface_desc_len] = {(uint8_t)k_tc_iface_desc_len,
                                        (uint8_t)k_tc_dtype_iface,
                                        0U,
                                        0U,
                                        2U,
                                        (uint8_t)k_tc_class_msc,
                                        (uint8_t)k_tc_subclass_scsi,
                                        (uint8_t)k_tc_protocol_bbb,
                                        0U};
  TEST_ASSERT(internal_enum_iface_is_msc(iface));

  iface[k_t_iface_off_class] = (uint8_t)k_tc_class_hid; /* wrong class -> false. */
  TEST_ASSERT(!internal_enum_iface_is_msc(iface));

  iface[k_t_iface_off_class] = (uint8_t)k_tc_class_msc;
  iface[6]                   = (uint8_t)k_tc_sub_rbc; /* wrong subclass -> false. */
  TEST_ASSERT(!internal_enum_iface_is_msc(iface));

  iface[6]                      = (uint8_t)k_tc_subclass_scsi;
  iface[k_t_iface_off_protocol] = (uint8_t)k_tc_proto_cbi; /* wrong protocol -> false. */
  TEST_ASSERT(!internal_enum_iface_is_msc(iface));

  TEST_END("iface_is_msc: true only for class 0x08 / sub 0x06 / proto 0x50");
}

/**
 * @brief Walk a rich config blob (non-MSC iface + stray EP + MSC bulk pair).
 * @pre The enumeration state has been reset.
 * @post The MSC bulk IN/OUT endpoints were discovered.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void walk_cfg_rich_blob(void)
{
  s_usb_hmsc_state.device         = (ra8_usb_hmsc_device_t){};
  uint8_t  blob[k_t_cfg_blob_cap] = {};
  uint16_t o                      = 0U;
  /* CONFIGURATION header. */
  blob[o + 0U] = (uint8_t)k_tc_cfg_hdr_len;
  blob[o + 1U] = (uint8_t)k_tc_dtype_config;
  o            = (uint16_t)(o + k_tc_cfg_hdr_len);
  /* Non-MSC interface (HID). */
  blob[o + 0U]                  = (uint8_t)k_tc_iface_desc_len;
  blob[o + 1U]                  = (uint8_t)k_tc_dtype_iface;
  blob[o + k_t_iface_off_class] = (uint8_t)k_tc_class_hid;
  o                             = (uint16_t)(o + k_tc_iface_desc_len);
  /* Endpoint while NOT in an MSC scope -> skipped. */
  blob[o + 0U] = (uint8_t)k_tc_ep_desc_len;
  blob[o + 1U] = (uint8_t)k_tc_dtype_endpoint;
  blob[o + 2U] = (uint8_t)k_tc_ep_in_addr;
  blob[o + 3U] = (uint8_t)k_tc_attr_bulk;
  o            = (uint16_t)(o + k_tc_ep_desc_len);
  /* MSC interface + bulk IN + bulk OUT. */
  blob[o + 0U]                     = (uint8_t)k_tc_iface_desc_len;
  blob[o + 1U]                     = (uint8_t)k_tc_dtype_iface;
  blob[o + k_t_iface_off_class]    = (uint8_t)k_tc_class_msc;
  blob[o + 6U]                     = (uint8_t)k_tc_subclass_scsi;
  blob[o + k_t_iface_off_protocol] = (uint8_t)k_tc_protocol_bbb;
  o                                = (uint16_t)(o + k_tc_iface_desc_len);
  blob[o + 0U]                     = (uint8_t)k_tc_ep_desc_len;
  blob[o + 1U]                     = (uint8_t)k_tc_dtype_endpoint;
  blob[o + 2U]                     = (uint8_t)k_tc_ep_in_addr;
  blob[o + 3U]                     = (uint8_t)k_tc_attr_bulk;
  blob[o + 4U]                     = (uint8_t)k_tc_mps_lo;
  o                                = (uint16_t)(o + k_tc_ep_desc_len);
  blob[o + 0U]                     = (uint8_t)k_tc_ep_desc_len;
  blob[o + 1U]                     = (uint8_t)k_tc_dtype_endpoint;
  blob[o + 2U]                     = (uint8_t)k_tc_ep_out_addr;
  blob[o + 3U]                     = (uint8_t)k_tc_attr_bulk;
  blob[o + 4U]                     = (uint8_t)k_tc_mps_lo;
  o                                = (uint16_t)(o + k_tc_ep_desc_len);
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_walk_cfg(blob, o));
  TEST_ASSERT_EQ(1U, s_usb_hmsc_state.device.bulk_in_ep);
  TEST_ASSERT_EQ(2U, s_usb_hmsc_state.device.bulk_out_ep);
}

/**
 * @brief Walk an MSC config with a bulk IN but no bulk OUT (hw_error).
 * @pre None.
 * @post `internal_enum_walk_cfg` reported the missing bulk OUT.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void walk_cfg_in_only(void)
{
  s_usb_hmsc_state.device             = (ra8_usb_hmsc_device_t){};
  uint8_t  in_only[32]                = {};
  uint16_t p                          = 0U;
  in_only[p + 0U]                     = (uint8_t)k_tc_iface_desc_len;
  in_only[p + 1U]                     = (uint8_t)k_tc_dtype_iface;
  in_only[p + k_t_iface_off_class]    = (uint8_t)k_tc_class_msc;
  in_only[p + 6U]                     = (uint8_t)k_tc_subclass_scsi;
  in_only[p + k_t_iface_off_protocol] = (uint8_t)k_tc_protocol_bbb;
  p                                   = (uint16_t)(p + k_tc_iface_desc_len);
  in_only[p + 0U]                     = (uint8_t)k_tc_ep_desc_len;
  in_only[p + 1U]                     = (uint8_t)k_tc_dtype_endpoint;
  in_only[p + 2U]                     = (uint8_t)k_tc_ep_in_addr;
  in_only[p + 3U]                     = (uint8_t)k_tc_attr_bulk;
  p                                   = (uint16_t)(p + k_tc_ep_desc_len);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_enum_walk_cfg(in_only, p));
}

/**
 * @brief Walk an MSC config with a bulk OUT but no bulk IN (hw_error).
 * @pre None.
 * @post `internal_enum_walk_cfg` reported the missing bulk IN.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void walk_cfg_out_only(void)
{
  s_usb_hmsc_state.device              = (ra8_usb_hmsc_device_t){};
  uint8_t  out_only[32]                = {};
  uint16_t q                           = 0U;
  out_only[q + 0U]                     = (uint8_t)k_tc_iface_desc_len;
  out_only[q + 1U]                     = (uint8_t)k_tc_dtype_iface;
  out_only[q + k_t_iface_off_class]    = (uint8_t)k_tc_class_msc;
  out_only[q + 6U]                     = (uint8_t)k_tc_subclass_scsi;
  out_only[q + k_t_iface_off_protocol] = (uint8_t)k_tc_protocol_bbb;
  q                                    = (uint16_t)(q + k_tc_iface_desc_len);
  out_only[q + 0U]                     = (uint8_t)k_tc_ep_desc_len;
  out_only[q + 1U]                     = (uint8_t)k_tc_dtype_endpoint;
  out_only[q + 2U]                     = (uint8_t)k_tc_ep_out_addr;
  out_only[q + 3U]                     = (uint8_t)k_tc_attr_bulk;
  q                                    = (uint16_t)(q + k_tc_ep_desc_len);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_enum_walk_cfg(out_only, q));
}

/**
 * @test test_walk_cfg_success_and_errors
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the descriptor stride uses
 * nested single-condition guards, not `&&`/`||`.)
 */
static void test_walk_cfg_success_and_errors(void)
{
  TEST_BEGIN("walk_cfg: finds MSC bulk pair; reports missing IN/OUT and zero len");

  /* Rich blob: a non-MSC interface + stray endpoint precede the MSC interface
   * so the "not in MSC" and "interface not MSC" legs are walked before the
   * matching pair. */
  reset_state();
  walk_cfg_rich_blob();

  /* IN present, OUT absent -> hw_error (missing bulk OUT). */
  walk_cfg_in_only();

  /* OUT present, IN absent -> hw_error (missing bulk IN). */
  walk_cfg_out_only();

  /* A zero-length descriptor breaks the walk before any endpoint is found. */
  s_usb_hmsc_state.device = (ra8_usb_hmsc_device_t){};
  uint8_t zero_len[16]    = {};
  zero_len[0]             = 0U; /* bLength == 0 -> break. */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_enum_walk_cfg(zero_len, (uint16_t)sizeof zero_len));

  TEST_END("walk_cfg: finds MSC bulk pair; reports missing IN/OUT and zero len");
}

/**
 * @test test_read_config_clamp_and_normal
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the buffer clamp is a
 * single-condition guard.)
 */
static void test_read_config_clamp_and_normal(void)
{
  TEST_BEGIN("read_config: normal total and oversized-total clamp both parse");

  /* Normal total: the canonical 32-byte blob, wTotalLength within the cap. */
  reset_state();
  s_usb_hmsc_state.device = (ra8_usb_hmsc_device_t){};
  uint8_t cfgval          = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_read_config(&cfgval));
  TEST_ASSERT_EQ(k_tc_cfg_value, cfgval);
  TEST_ASSERT_EQ(1U, s_usb_hmsc_state.device.bulk_in_ep);

  /* Oversized total: wTotalLength claims 200 (> 128 buffer) so the clamp
   * fires; the walk still succeeds on the real descriptor bytes. */
  reset_state();
  s_usb_hmsc_state.device    = (ra8_usb_hmsc_device_t){};
  s_cfg_blob[k_tc_off_total] = (uint8_t)k_tc_clamp_total; /* lie about length. */
  uint8_t cfgval2            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_read_config(&cfgval2));
  TEST_ASSERT_EQ(k_tc_cfg_value, cfgval2);

  TEST_END("read_config: normal total and oversized-total clamp both parse");
}

/**
 * @test test_configure_lun_variants
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the GET_MAX_LUN success is
 * two nested single-condition guards.)
 */
static void test_configure_lun_variants(void)
{
  TEST_BEGIN("configure: GET_MAX_LUN value taken only on ok + 1-byte response");

  /* GET_MAX_LUN answers ok with a 1-byte payload -> max_lun latches it. */
  reset_state();
  s_usb_hmsc_state.device.bulk_in_ep  = 1U;
  s_usb_hmsc_state.device.bulk_out_ep = 2U;
  s_lun_err                           = k_ra8_ok;
  s_lun_rx                            = 1U;
  s_lun_val                           = (uint8_t)k_tc_lun_val;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_configure(1U, (uint8_t)k_tc_cfg_value));
  TEST_ASSERT_EQ(k_tc_lun_val, s_usb_hmsc_state.device.max_lun);

  /* GET_MAX_LUN stalls (error) -> LUN defaults to 0. */
  reset_state();
  s_usb_hmsc_state.device.bulk_in_ep  = 1U;
  s_usb_hmsc_state.device.bulk_out_ep = 2U;
  s_lun_err                           = k_ra8_err_hw_error;
  s_lun_val                           = (uint8_t)k_tc_lun_val;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_configure(1U, (uint8_t)k_tc_cfg_value));
  TEST_ASSERT_EQ(0U, s_usb_hmsc_state.device.max_lun);

  /* GET_MAX_LUN ok but returns zero bytes -> LUN defaults to 0. */
  reset_state();
  s_usb_hmsc_state.device.bulk_in_ep  = 1U;
  s_usb_hmsc_state.device.bulk_out_ep = 2U;
  s_lun_err                           = k_ra8_ok;
  s_lun_rx                            = 0U;
  s_lun_val                           = (uint8_t)k_tc_lun_val;
  TEST_ASSERT_EQ(k_ra8_ok, internal_enum_configure(1U, (uint8_t)k_tc_cfg_value));
  TEST_ASSERT_EQ(0U, s_usb_hmsc_state.device.max_lun);

  TEST_END("configure: GET_MAX_LUN value taken only on ok + 1-byte response");
}

/**
 * @test test_fill_ids_little_endian
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- pure little-endian field
 * unpack.)
 */
static void test_fill_ids_little_endian(void)
{
  TEST_BEGIN("fill_ids: unpacks little-endian idVendor / idProduct");
  reset_state();
  s_usb_hmsc_state.device = (ra8_usb_hmsc_device_t){};

  internal_enum_fill_ids(s_dev_desc);
  TEST_ASSERT_EQ(k_tc_vid, s_usb_hmsc_state.device.vendor_id);
  TEST_ASSERT_EQ(k_tc_pid, s_usb_hmsc_state.device.product_id);

  TEST_END("fill_ids: unpacks little-endian idVendor / idProduct");
}

/**
 * @test test_publish_callback_and_copy
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the callback and out-copy
 * are independent single-condition null guards.)
 */
static void test_publish_callback_and_copy(void)
{
  TEST_BEGIN("publish: fires callback + copies snapshot; tolerates both NULL");

  /* Callback registered + destination provided -> both fire. */
  reset_state();
  s_usb_hmsc_state.attach_cb         = cov_on_attach;
  s_usb_hmsc_state.attach_ctx        = (void*)k_tc_ctx_token;
  s_usb_hmsc_state.device.bulk_in_ep = 1U;
  ra8_usb_hmsc_device_t out          = {};
  internal_enum_publish(1U, &out);
  TEST_ASSERT_EQ(1U, s_attach_count);
  TEST_ASSERT(s_attach_ctx_seen == (void*)k_tc_ctx_token);
  TEST_ASSERT(s_usb_hmsc_state.attached);
  TEST_ASSERT_EQ(1U, out.device_address);

  /* No callback + NULL destination -> no crash, still flips attached. */
  reset_state();
  s_usb_hmsc_state.attach_cb = nullptr;
  internal_enum_publish(1U, nullptr);
  TEST_ASSERT_EQ(0U, s_attach_count);
  TEST_ASSERT(s_usb_hmsc_state.attached);
  TEST_ASSERT_EQ(1U, s_usb_hmsc_state.device.device_address);

  TEST_END("publish: fires callback + copies snapshot; tolerates both NULL");
}

/**
 * @test test_enumerate_full_success
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the ladder is a sequence of
 * single-condition error short-circuits.)
 *
 * @note End-to-end run of the renamed `ra8_usb_hmsc_enumerate_cov`: the mocks
 * present a valid attach, a full device descriptor, a parseable MSC
 * configuration and a well-behaved GET_MAX_LUN, so the whole ladder
 * (hunt -> fill_ids -> assign_addr -> read_config -> configure -> publish)
 * completes and the attach callback fires.
 */
static void test_enumerate_full_success(void)
{
  TEST_BEGIN("enumerate: full ladder succeeds and publishes the device");
  reset_state();
  s_usb_hmsc_state.attach_cb  = cov_on_attach;
  s_usb_hmsc_state.attach_ctx = (void*)k_tc_ctx_token;

  ra8_usb_hmsc_device_t dev = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_hmsc_enumerate_cov(&dev));
  TEST_ASSERT(s_usb_hmsc_state.attached);
  TEST_ASSERT_EQ(1U, s_attach_count);
  TEST_ASSERT_EQ(1U, dev.device_address);
  TEST_ASSERT_EQ(1U, dev.bulk_in_ep);
  TEST_ASSERT_EQ(2U, dev.bulk_out_ep);
  TEST_ASSERT_EQ(k_tc_vid, dev.vendor_id);
  TEST_ASSERT_EQ(k_tc_pid, dev.product_id);

  TEST_END("enumerate: full ladder succeeds and publishes the device");
}

/**
 * @test test_enumerate_ladder_failure_and_guard
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition guards.)
 *
 * @note Two negative legs: (1) the init guard rejects an enumerate before
 * init, and (2) a ladder whose device never answers propagates the hunt's
 * timeout out of `ra8_usb_hmsc_enumerate_cov` without firing the callback.
 */
static void test_enumerate_ladder_failure_and_guard(void)
{
  TEST_BEGIN("enumerate: pre-init guard + hunt-timeout propagation");

  /* Pre-init guard. */
  reset_state();
  s_usb_hmsc_state.initialized = false;
  ra8_usb_hmsc_device_t dev    = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_hmsc_enumerate_cov(&dev));

  /* Ladder failure: no descriptor ever answers -> hunt timeout out the top. */
  reset_state();
  s_usb_hmsc_state.attach_cb = cov_on_attach;
  s_line_state               = 1U;
  s_dev_err                  = k_ra8_err_hw_timeout;
  TEST_ASSERT(ra8_usb_hmsc_enumerate_cov(&dev) != k_ra8_ok);
  TEST_ASSERT_EQ(0U, s_attach_count);
  TEST_ASSERT(!s_usb_hmsc_state.attached);

  TEST_END("enumerate: pre-init guard + hunt-timeout propagation");
}

int32_t main(void)
{
  test_read_dev_desc_short_and_full();
  test_hunt_success_and_attach_timeout();
  test_assign_addr_both_legs();
  test_note_endpoint_all_slots();
  test_iface_is_msc_all_fields();
  test_walk_cfg_success_and_errors();
  test_read_config_clamp_and_normal();
  test_configure_lun_variants();
  test_fill_ids_little_endian();
  test_publish_callback_and_copy();
  test_enumerate_full_success();
  test_enumerate_ladder_failure_and_guard();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_hmsc_enum_cov.c\n");
  return 0;
}
