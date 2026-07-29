/**
 * @file test_ra8_c6link.c
 * @brief The `ra8_c6link` facade against a co-processor model (#490).
 *
 * @details
 * Every test here drives the whole stack -- payload header, checksum, TLV
 * envelope, protobuf encode, transaction pump, protobuf decode, UID
 * correlation -- against `tests/mocks/ra8_c6_model.c`, which decodes what the
 * host sends with the same generated codec the ESP32-C6 runs and synthesises
 * the answer the co-processor would send. No hardware is involved and none is
 * simulated: what is modelled is the protocol, which is the part that can be
 * wrong.
 *
 * The pure layers underneath (arena, payload header, envelope) are tested on
 * their own in `test_ra8_c6link_wire.c`, so a failure here is unambiguous.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_c6_model.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_c6link_wifi.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum t_c6_const_t
 * @brief Sizes this test file owns, as opposed to the model's own.
 */
typedef enum : uint32_t {
  k_t_arena          = 4096U, /**< Decode arena handed to the link.                  */
  k_t_events         = 8U,    /**< Announcements this file will remember.            */
  k_t_ssid_len       = 9U,    /**< Length of the SSID the tests configure.           */
  k_t_stray_len      = 16U,   /**< Length of a payload that is not an envelope.      */
  k_t_junk_len       = 3U,    /**< Length of an envelope body that is not an Rpc.    */
  k_t_hdr_off_lo     = 4U,    /**< Offset of the payload header's offset field.      */
  k_t_pin_first      = 0xDEU, /**< First octet of the pinned BSSID a join sends.     */
  k_t_pin_last       = 0xADU, /**< Its last octet.                                   */
  k_t_pin_last_i     = 5U,    /**< Index of that last octet.                         */
  k_t_stray_first    = 0xF0U, /**< First octet of a payload that is not an envelope. */
  k_t_junk_octet     = 0xFFU, /**< Filler for an envelope body that is not an Rpc.   */
  k_t_caps_tlv_bytes = 15U,   /**< Octets of TLV the announcement declares.          */
  k_t_throttle_high  = 80U,   /**< Flow-control high-water mark it advertises.       */
  k_t_throttle_low   = 60U,   /**< Its low-water mark.                               */
} t_c6_const_t;

/** @brief Decode arena handed to the link under test. */
static uint8_t s_arena[(size_t)k_t_arena];

/** @brief The link under test. */
static ra8_c6link_t s_link;

/** @brief Announcements the link delivered, oldest first. */
static ra8_c6link_event_t s_events[k_t_events];

/** @brief Number of entries in ::s_events. */
static uint8_t s_event_n;

/** @brief Length of the last 802.3 frame the link delivered inbound. */
static uint16_t s_rx_len;

/**
 * @brief A binary field too large for one frame, for the refusal path.
 * @details File scope because the request that carries it must outlive the
 * assertion, and because a frame-sized array is not a stack object this tree
 * wants inside a test function.
 * @note Never transmitted: the request carrying it is refused before staging.
 * @warning Its size must stay above ::k_ra8_c6link_max_payload.
 * @since 0.1.0
 */
static uint8_t s_oversize[k_ra8_c6link_frame_bytes];

/**
 * @brief Record an announcement the link delivered.
 * @param[in] ctx Unused.
 * @param[in] ev The announcement; never null.
 * @return Nothing.
 */
static void t_on_event(void* ctx, const ra8_c6link_event_t* ev)
{
  (void)ctx;
  if (s_event_n < (uint8_t)k_t_events) {
    s_events[s_event_n] = *ev;
    s_event_n++;
  }
}

/**
 * @brief Record an inbound 802.3 frame the link delivered.
 * @param[in] ctx Unused.
 * @param[in] frame The frame; never null.
 * @param[in] len Its length.
 * @return Nothing.
 */
static void t_on_rx(void* ctx, const uint8_t* frame, uint16_t len)
{
  (void)ctx;
  TEST_ASSERT_NOT_NULL(frame);
  s_rx_len = len;
}

/** @brief Fill a configuration that ::ra8_c6link_open will accept. */
static void t_cfg(ra8_c6link_cfg_t* cfg)
{
  *cfg = (ra8_c6link_cfg_t){};
  ra8_c6_model_bind(&cfg->transport);
  cfg->arena       = s_arena;
  cfg->arena_bytes = (uint32_t)k_t_arena;
  cfg->event_cb    = t_on_event;
  cfg->rx_cb       = t_on_rx;
}

/** @brief Reset the model and open a link against it. */
static void t_reset(void)
{
  ra8_c6_model_reset();
  s_event_n = 0U;
  s_rx_len  = 0U;
  s_link    = (ra8_c6link_t){};

  ra8_c6link_cfg_t cfg = {};
  t_cfg(&cfg);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_open(&s_link, &cfg));
}

/**
 * @brief Reset, open, and bring the link to ready -- the mandatory bring-up.
 * @details The co-processor services no request until the host has announced
 * itself on `ESP_PRIV_IF`, so every test that issues one starts here rather
 * than at ::t_reset. Both logs are rewound afterwards -- the announcements
 * seen here and the requests the readiness probe itself issued -- so a test
 * observes only what it provoked.
 */
static void t_bringup(void)
{
  t_reset();
  ra8_c6link_fw_version_t fw = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_await_ready(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &fw));
  TEST_ASSERT(ra8_c6_model()->caps_seen);
  TEST_ASSERT_EQ(k_c6m_chip_id, fw.chip_id);
  s_event_n              = 0U;
  ra8_c6_model()->seen_n = 0U;
}

/**
 * @par MC/DC:
 * Decision: `(transfer == nullptr) || (handshake_active == nullptr) ||
 *            (delay_ms == nullptr) || (arena == nullptr)` (4 conditions)
 * - Vector 1: all four supplied      -> false (control)
 * - Vector 2: transfer NULL          -> true  (varies transfer only)
 * - Vector 3: handshake_active NULL  -> true  (varies handshake only)
 * - Vector 4: delay_ms NULL          -> true  (varies delay only)
 * - Vector 5: arena NULL             -> true  (varies arena only)
 * Vector 1 paired with each of 2..5 proves the corresponding condition
 * independently decides. N+1 = 5 vectors for N=4: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_check_cfg
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_open
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_is_open
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_last_fault
 */
static void test_open_validation(void)
{
  TEST_BEGIN("c6link open validation");
  ra8_c6_model_reset();
  ra8_c6link_t     link = {};
  ra8_c6link_cfg_t cfg  = {};
  t_cfg(&cfg);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_open(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_open(&link, nullptr));

  ra8_c6link_cfg_t broken   = cfg;
  broken.transport.transfer = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_open(&link, &broken));
  broken                            = cfg;
  broken.transport.handshake_active = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_open(&link, &broken));
  broken                    = cfg;
  broken.transport.delay_ms = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_open(&link, &broken));
  broken       = cfg;
  broken.arena = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_open(&link, &broken));

  broken             = cfg;
  broken.arena_bytes = (uint32_t)k_ra8_c6link_arena_min - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_open(&link, &broken));

  TEST_ASSERT(!ra8_c6link_is_open(&link));
  TEST_ASSERT(!ra8_c6link_is_open(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_open(&link, &cfg));
  TEST_ASSERT(ra8_c6link_is_open(&link));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_c6link_open(&link, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&link));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_c6link_close(&link));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_close(nullptr));
  TEST_END("c6link open validation");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- one request is encoded, decoded by the
 * model, answered, and the answer decoded back; every field is compared)
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_fw_version
 */
static void test_fw_version_roundtrip(void)
{
  TEST_BEGIN("c6link identity round trip");
  t_bringup();
  ra8_c6link_fw_version_t fw = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_fw_version(&s_link, &fw));
  TEST_ASSERT_EQ(k_c6m_fw_major, fw.major);
  TEST_ASSERT_EQ(k_c6m_fw_minor, fw.minor);
  TEST_ASSERT_EQ(k_c6m_fw_patch, fw.patch);
  TEST_ASSERT_EQ(k_c6m_chip_id, fw.chip_id);
  TEST_ASSERT_EQ(0, strcmp(fw.target, "esp32c6"));
  TEST_ASSERT_EQ(1, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_GetCoprocessorFwVersion, ra8_c6_model()->seen[0]);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_fw_version(nullptr, &fw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_fw_version(&s_link, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_c6link_fw_version(&s_link, &fw));
  TEST_END("c6link identity round trip");
}

/**
 * @par MC/DC:
 * Decision: `!armed || (msg->uid != wait.uid) || (msg->msg_id != wait.resp_id)`
 * (3 conditions)
 * - Vector 1: armed, UID matches, id matches -> false (control: answer taken)
 * - Vector 2: not armed -- an unsolicited answer with no request outstanding
 *                                            -> true (varies armed only)
 * - Vector 3: armed, UID differs, id matches -> true (varies UID only)
 * - Vector 4: armed, UID matches, id differs -> true (varies id only)
 * Vector 1 paired with each of 2, 3 and 4 proves the corresponding condition
 * independently decides. N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@ra8_c6link_rpc_answer
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@ra8_c6link_priv_rpc_consume
 */
static void test_answer_correlation(void)
{
  TEST_BEGIN("c6link answer correlation");
  ra8_c6link_fw_version_t fw = {};

  t_bringup();
  ra8_c6_model()->wrong_uid = true;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(&s_link, &fw));

  t_bringup();
  ra8_c6_model()->wrong_id = true;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(&s_link, &fw));

  t_bringup();
  ra8_c6_model()->mute = true;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(&s_link, &fw));

  /* An answer with no request outstanding must be counted and dropped, not
     acted on: the link is only polled here, never asked anything. */
  t_bringup();
  ra8_c6_model_emit_stray();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 2U, &stats));
  TEST_ASSERT_EQ(1, stats.rpc_in);
  TEST_ASSERT_EQ(0, stats.events);
  TEST_END("c6link answer correlation");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the three start requests are observed in
 * order, and a scripted refusal of the middle one stops the sequence there)
 */
static void test_wifi_start_sequence(void)
{
  TEST_BEGIN("c6link station start sequence");
  t_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_start(&s_link));
  TEST_ASSERT_EQ(3, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiInit, ra8_c6_model()->seen[0]);
  TEST_ASSERT_EQ(RPC_ID__Req_SetWifiMode, ra8_c6_model()->seen[1]);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiStart, ra8_c6_model()->seen[2]);

  ra8_c6link_fault_t fault = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(&s_link, &fault));
  TEST_ASSERT_EQ(0, fault.rpc_id);

  t_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_SetWifiMode;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_start(&s_link));
  TEST_ASSERT_EQ(2, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(&s_link, &fault));
  TEST_ASSERT_EQ(RPC_ID__Req_SetWifiMode, fault.rpc_id);
  TEST_ASSERT_EQ(k_c6m_esp_fail, fault.resp);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_last_fault(&s_link, nullptr));

  /* Teardown attempts both steps even when the first is refused: a
     co-processor left half-configured is worse than a reported fault. */
  t_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiStop;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_stop(&s_link));
  TEST_ASSERT_EQ(2, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiDeinit, ra8_c6_model()->seen[1]);

  t_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_stop(&s_link));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_start(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_stop(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_leave(nullptr));
  TEST_END("c6link station start sequence");
}

/**
 * @par MC/DC:
 * Decision: `(ssid_len == 0) || (ssid_len > 32) || (pass_len > 64)`
 * (3 conditions, inside ::ra8_c6link_wifi_join)
 * - Vector 1: ssid 9, pass 14  -> false (control: the join is issued)
 * - Vector 2: ssid 0, pass 14  -> true  (varies the zero test only)
 * - Vector 3: ssid 33, pass 14 -> true  (varies the SSID maximum only)
 * - Vector 4: ssid 9, pass 65  -> true  (varies the passphrase maximum only)
 * Vector 1 paired with each of 2, 3 and 4 proves the corresponding condition
 * independently decides. N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_wifi_sta.c@ra8_c6link_wifi_join
 */
static void test_join_credentials(void)
{
  TEST_BEGIN("c6link credentials reach the co-processor");
  t_bringup();
  ra8_c6link_sta_cfg_t sta = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_sta_cfg_set(&sta, "ra8-bench", "hunter2hunter2"));
  sta.channel = (uint8_t)k_c6m_channel;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_join(&s_link, &sta));
  TEST_ASSERT_EQ(2, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiSetConfig, ra8_c6_model()->seen[0]);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiConnect, ra8_c6_model()->seen[1]);
  TEST_ASSERT_EQ(0, strcmp(ra8_c6_model()->ssid, "ra8-bench"));
  TEST_ASSERT_EQ(0, strcmp(ra8_c6_model()->pass, "hunter2hunter2"));

  ra8_c6link_sta_cfg_t bad = sta;
  bad.ssid_len             = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_wifi_join(&s_link, &bad));
  bad          = sta;
  bad.ssid_len = (uint8_t)k_ra8_c6link_ssid_max + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_wifi_join(&s_link, &bad));
  bad          = sta;
  bad.pass_len = (uint8_t)k_ra8_c6link_pass_max + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_wifi_join(&s_link, &bad));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_join(&s_link, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_join(nullptr, &sta));

  t_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_leave(&s_link));
  TEST_ASSERT_EQ(RPC_ID__Req_WifiDisconnect, ra8_c6_model()->seen[0]);
  TEST_END("c6link credentials reach the co-processor");
}

/**
 * @par MC/DC:
 * Decision: `(ssid_len == 0) || (ssid_len > k_ra8_c6link_ssid_max)`
 * (2 conditions, inside ::ra8_c6link_sta_cfg_set)
 * - Vector 1: a nine-character SSID  -> false (control)
 * - Vector 2: an empty SSID          -> true  (varies the zero test only)
 * - Vector 3: a 33-character SSID    -> true  (varies the maximum test only)
 * Vectors 1+2 and 1+3 prove each condition independently decides.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_wifi_sta.c@ra8_c6link_sta_cfg_set
 * Decisions: libs/ra8_c6link/src/ra8_c6link_wifi_sta.c@ra8_c6link_sta_len
 */
static void test_sta_cfg_set(void)
{
  TEST_BEGIN("c6link station configuration builder");
  ra8_c6link_sta_cfg_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_sta_cfg_set(nullptr, "x", nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_sta_cfg_set(&cfg, nullptr, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_sta_cfg_set(&cfg, "ra8-bench", nullptr));
  TEST_ASSERT_EQ(k_t_ssid_len, cfg.ssid_len);
  TEST_ASSERT_EQ(0, cfg.pass_len);

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_sta_cfg_set(&cfg, "", nullptr));

  char too_long[(size_t)k_ra8_c6link_ssid_max + 2U];
  (void)memset(too_long, 'a', sizeof too_long - 1U);
  too_long[sizeof too_long - 1U] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_sta_cfg_set(&cfg, too_long, nullptr));

  char long_pass[(size_t)k_ra8_c6link_pass_max + 2U];
  (void)memset(long_pass, 'b', sizeof long_pass - 1U);
  long_pass[sizeof long_pass - 1U] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_sta_cfg_set(&cfg, "ra8-bench", long_pass));
  TEST_END("c6link station configuration builder");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the address and the AP record are
 * decoded from real protobuf answers and compared field by field)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_wifi_sta.c@ra8_c6link_wifi_mac
 * Decisions: libs/ra8_c6link/src/ra8_c6link_wifi_sta.c@ra8_c6link_wifi_ap_info
 */
static void test_mac_and_ap_info(void)
{
  TEST_BEGIN("c6link station address and AP record");
  const uint8_t want[] = {9U, 8U, 7U, 6U, 5U, 4U};

  t_bringup();
  ra8_c6link_mac_t mac = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_mac(&s_link, &mac));
  TEST_ASSERT_EQ(0, memcmp(mac.octet, want, sizeof want));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_mac(&s_link, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_mac(nullptr, &mac));

  t_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_GetMACAddress;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_mac(&s_link, &mac));

  t_bringup();
  ra8_c6link_ap_info_t ap = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_ap_info(&s_link, &ap));
  TEST_ASSERT_EQ(0, strcmp(ap.ssid, "benc"));
  TEST_ASSERT_EQ(k_c6m_channel, ap.channel);
  TEST_ASSERT_EQ(-(int32_t)k_c6m_rssi_mag, ap.rssi);
  TEST_ASSERT_EQ(0, memcmp(ap.bssid.octet, want, sizeof want));

  /* An unassociated station is what a refusal here means, and it must surface
     as a protocol error carrying the co-processor's own code. */
  t_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiStaGetApInfo;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_ap_info(&s_link, &ap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_ap_info(nullptr, &ap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_ap_info(&s_link, nullptr));
  TEST_END("c6link station address and AP record");
}

/**
 * @par MC/DC:
 * Decision: `if ((link == nullptr) || (out == nullptr))` (2 conditions)
 * - Vector 1: link=valid, out=valid -> false (control: both conditions false)
 * - Vector 2: link=NULL,  out=valid -> true  (varies link only)
 * - Vector 3: link=valid, out=NULL  -> true  (varies out only)
 * Vectors 1+2 prove link independently affects the outcome; 1+3 prove the same
 * for out. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * The surrounding case is the readiness contract itself: a boot announcement is
 * delivered when the co-processor sends one, and readiness is established
 * whether or not it does.
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_await_ready
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_priv_emit
 */
static void test_events(void)
{
  TEST_BEGIN("c6link announcements");
  t_reset();
  ra8_c6link_fw_version_t fw = {};
  /* The announcement is what provokes the boot event; the model answers it the
     way the silicon does rather than the test queueing one by hand. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_await_ready(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &fw));
  TEST_ASSERT(ra8_c6_model()->caps_seen);
  TEST_ASSERT_EQ(1, s_event_n);
  TEST_ASSERT_EQ(k_ra8_c6link_event_boot, s_events[0].kind);

  ra8_c6_model_emit_disconnected();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 4U, &stats));
  TEST_ASSERT_EQ(2, s_event_n);
  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_disconnected, s_events[1].kind);
  TEST_ASSERT_EQ(k_c6m_reason, s_events[1].reason);
  TEST_ASSERT_EQ(-(int32_t)k_c6m_rssi_mag, s_events[1].rssi);
  TEST_ASSERT_EQ(0, strcmp(s_events[1].ssid, "benc"));

  /* THE REGRESSION THIS ENTRY POINT EXISTS FOR. `Event_ESPInit` fires once,
     when the CO-PROCESSOR boots. The C6 on the bench has its own supply, so
     resetting this host does not reboot it and that event is long gone -- it
     was consumed by whichever application was clocking the bus when it fired.
     A link that treated the event as its readiness signal worked exactly once,
     on a freshly-flashed co-processor, and timed out on every run after. With
     the model emitting no boot event at all, readiness must still be
     established and the identity must still come back. */
  t_reset();
  ra8_c6_model()->silent_boot   = true;
  ra8_c6link_fw_version_t quiet = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_await_ready(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &quiet));
  TEST_ASSERT_EQ(0, s_event_n);
  TEST_ASSERT_EQ(k_c6m_chip_id, quiet.chip_id);
  TEST_ASSERT(ra8_c6_model()->caps_seen);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_await_ready(nullptr, 2U, &quiet));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_await_ready(&s_link, 2U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_await_ready(&s_link, 0U, &quiet));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_c6link_await_ready(&s_link, 2U, &quiet));
  TEST_END("c6link announcements");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the two remaining announcement kinds are
 * decoded from real protobuf events and compared field by field)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@ra8_c6link_rpc_event
 */
static void test_events_remaining(void)
{
  TEST_BEGIN("c6link association announcements");
  const uint8_t want_bssid[] = {2U, 3U, 4U, 5U, 6U, 7U};

  t_reset();
  ra8_c6_model_emit_connected();
  ra8_c6_model_emit_wifi_event();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 4U, &stats));
  TEST_ASSERT_EQ(2, s_event_n);
  TEST_ASSERT_EQ(2, stats.events);

  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_connected, s_events[0].kind);
  TEST_ASSERT_EQ(0, strcmp(s_events[0].ssid, "benc"));
  TEST_ASSERT_EQ(k_c6m_channel, s_events[0].channel);
  TEST_ASSERT_EQ(0, memcmp(s_events[0].bssid.octet, want_bssid, sizeof want_bssid));

  /* The bare Wi-Fi event is the shape the co-processor raises most often: the
     bench run saw WIFI_EVENT_STA_START and WIFI_EVENT_STA_STOP arrive this
     way, carrying nothing but their own id. */
  TEST_ASSERT_EQ(k_ra8_c6link_event_wifi, s_events[1].kind);
  TEST_ASSERT_EQ(k_c6m_wifi_ev, s_events[1].wifi_event_id);
  TEST_ASSERT_EQ(0, s_events[1].ssid_len);
  TEST_END("c6link association announcements");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- a pinned BSSID reaches the co-processor
 * as a six-octet field, and a request id that is not a bare one is refused)
 */
static void test_join_pinned_bssid(void)
{
  TEST_BEGIN("c6link pinned BSSID and unknown bare request");
  t_bringup();
  ra8_c6link_sta_cfg_t sta = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_sta_cfg_set(&sta, "ra8-bench", "hunter2hunter2"));
  sta.bssid_set                   = true;
  sta.bssid.octet[0]              = (uint8_t)k_t_pin_first;
  sta.bssid.octet[k_t_pin_last_i] = (uint8_t)k_t_pin_last;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_join(&s_link, &sta));
  TEST_ASSERT_EQ(RPC_ID__Req_WifiSetConfig, ra8_c6_model()->seen[0]);

  /* A request id this helper does not know is refused rather than encoded as
     an empty message the co-processor would have to guess at. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_c6link_priv_bare_req(&s_link, (uint32_t)RPC_ID__Req_WifiInit));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_priv_bare_req(nullptr, 0U));
  TEST_END("c6link pinned BSSID and unknown bare request");
}

/**
 * @par MC/DC:
 * Decision: `(len == 0) || (len > k_ra8_c6link_max_payload)` (2 conditions)
 * - Vector 1: len 64                 -> false (control: the frame is clocked)
 * - Vector 2: len 0                  -> true  (varies the zero test only)
 * - Vector 3: len max_payload + 1    -> true  (varies the maximum test only)
 * Vectors 1+2 and 1+3 prove each condition independently decides.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_eth_send
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_priv_dispatch
 */
static void test_eth_data_plane(void)
{
  TEST_BEGIN("c6link Ethernet data plane");
  t_bringup();
  uint8_t frame[(size_t)k_c6m_eth_len];
  for (uint16_t i = 0U; i < (uint16_t)k_c6m_eth_len; i++) {
    frame[i] = (uint8_t)(0x20U + i);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_eth_send(&s_link, frame, (uint16_t)k_c6m_eth_len));
  TEST_ASSERT_EQ(k_c6m_eth_len, ra8_c6_model()->eth_tx_len);
  TEST_ASSERT_EQ(0, memcmp(ra8_c6_model()->eth_tx, frame, sizeof frame));

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_eth_send(&s_link, frame, 0U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_c6link_eth_send(&s_link, frame, (uint16_t)((uint16_t)k_ra8_c6link_max_payload + 1U)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_eth_send(&s_link, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_eth_send(nullptr, frame, 1U));

  /* Inbound: a station frame reaches the receive callback. */
  t_bringup();
  ra8_c6_model_emit_eth();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 2U, &stats));
  TEST_ASSERT_EQ(k_c6m_eth_len, s_rx_len);
  TEST_ASSERT_EQ(1, stats.eth_in);

  /* A frame on an interface this link does not use is counted, not delivered.
     `ESP_PRIV_IF` is the real case: upstream reads peripheral-side capabilities from it,
     but this co-processor build's only privileged frame fails its own
     checksum (#529), so nothing here may depend on one. */
  t_bringup();
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  ra8_c6link_priv_frame_seal(slot, (uint8_t)ESP_PRIV_IF, 0U, 4U);
  stats = (ra8_c6link_stats_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 2U, &stats));
  TEST_ASSERT_EQ(1, stats.unrouted);
  TEST_ASSERT_EQ(0, s_rx_len);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_c6link_eth_send(&s_link, frame, (uint16_t)k_c6m_eth_len));
  TEST_END("c6link Ethernet data plane");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- an inactive handshake and a refusing
 * transport are each driven to their own return code)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_pump.c@ra8_c6link_priv_pump
 */
static void test_transport_faults(void)
{
  TEST_BEGIN("c6link transport faults");
  t_reset();
  ra8_c6_model()->handshake = false;
  ra8_c6link_stats_t stats  = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_c6link_poll(&s_link, 8U, &stats));
  TEST_ASSERT_EQ(0, stats.transfers);
  /* It gives up after three consecutive misses rather than spending the whole
     budget: an absent co-processor must produce a verdict, not a hang. */
  TEST_ASSERT_EQ(k_ra8_c6link_hs_giveup, stats.hs_timeouts);

  t_reset();
  ra8_c6_model()->fail_transfer = true;
  stats                         = (ra8_c6link_stats_t){};
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_c6link_poll(&s_link, 8U, &stats));

  t_reset();
  ra8_c6_model()->fail_transfer = true;
  ra8_c6link_fw_version_t fw    = {};
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_c6link_fw_version(&s_link, &fw));

  t_reset();
  ra8_c6_model()->fail_transfer        = true;
  uint8_t frame[(size_t)k_c6m_eth_len] = {};
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_c6link_eth_send(&s_link, frame, (uint16_t)k_c6m_eth_len));

  t_reset();
  ra8_c6_model()->handshake = false;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_c6link_eth_send(&s_link, frame, (uint16_t)k_c6m_eth_len));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_poll(nullptr, 1U, nullptr));
  t_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_poll(&s_link, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_c6link_poll(&s_link, 1U, nullptr));
  TEST_END("c6link transport faults");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- a payload that is not an envelope and an
 * envelope whose body is not a message are each counted, not acted on)
 */
static void test_undecodable(void)
{
  TEST_BEGIN("c6link undecodable control frames");
  t_reset();
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  for (uint8_t i = 0U; i < (uint8_t)k_t_stray_len; i++) {
    slot[(uint8_t)k_ra8_c6link_header_bytes + i] = (uint8_t)((uint16_t)k_t_stray_first + i);
  }
  ra8_c6link_priv_frame_seal(slot, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_stray_len);

  slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  uint8_t* payload = &slot[k_ra8_c6link_header_bytes];
  uint16_t body_at = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_priv_tlv_open(payload,
                                          (uint16_t)k_ra8_c6link_max_payload,
                                          (uint16_t)k_t_junk_len,
                                          &body_at));
  for (uint8_t i = 0U; i < (uint8_t)k_t_junk_len; i++) {
    payload[body_at + i] = (uint8_t)k_t_junk_octet;
  }
  ra8_c6link_priv_frame_seal(slot,
                             (uint8_t)ESP_SERIAL_IF,
                             0U,
                             (uint16_t)(body_at + (uint16_t)k_t_junk_len));

  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 4U, &stats));
  TEST_ASSERT_EQ(2, stats.undecodable);
  TEST_ASSERT_EQ(0, s_event_n);
  TEST_END("c6link undecodable control frames");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- each malformed shape is counted in its
 * own bucket, and an announcement the facade does not model is dropped)
 */
static void test_rejected_frames(void)
{
  TEST_BEGIN("c6link rejected and unmodelled frames");
  t_reset();

  /* A header whose offset is not the payload-header size. */
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  ra8_c6link_priv_frame_seal(slot, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_junk_len);
  slot[k_t_hdr_off_lo] = 0U;

  /* A frame whose payload no longer matches its checksum. */
  slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  ra8_c6link_priv_frame_seal(slot, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_junk_len);
  slot[k_ra8_c6link_header_bytes]++;

  ra8_c6_model_emit_unmodelled_event();
  ra8_c6_model_emit_inbound_request();

  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 8U, &stats));
  TEST_ASSERT_EQ(1, stats.malformed);
  TEST_ASSERT_EQ(1, stats.bad_checksum);
  TEST_ASSERT_EQ(2, stats.rpc_in);
  /* The inbound request decoded fine and was still refused: "the codec would
     not parse it" and "a host has no handler for it" are different facts, and
     both land in `undecodable` only because neither is ever actionable. */
  TEST_ASSERT_EQ(1, stats.undecodable);
  TEST_ASSERT_EQ(0, s_event_n);

  /* An announcement whose optional inner message the co-processor omitted:
     protobuf permits it, so the decoder must report the kind and leave every
     other field zero rather than dereference an absent body. */
  t_reset();
  ra8_c6_model_emit_hollow_events();
  stats = (ra8_c6link_stats_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 4U, &stats));
  TEST_ASSERT_EQ(2, s_event_n);
  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_connected, s_events[0].kind);
  TEST_ASSERT_EQ(0, s_events[0].ssid_len);
  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_disconnected, s_events[1].kind);
  TEST_ASSERT_EQ(0, s_events[1].reason);
  TEST_END("c6link rejected and unmodelled frames");
}

/**
 * @par MC/DC:
 * Decision: `(link == nullptr) || (req == nullptr) || (take == nullptr)`
 * (3 conditions, inside ::ra8_c6link_priv_rpc_call)
 * - Vector 1: all three supplied -> false (control: the request is issued)
 * - Vector 2: link NULL          -> true  (varies link only)
 * - Vector 3: req NULL           -> true  (varies req only)
 * - Vector 4: take NULL          -> true  (varies take only)
 * Vector 1 paired with each of 2, 3 and 4 proves the corresponding condition
 * independently decides. N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@ra8_c6link_priv_rpc_call
 */
static void test_rpc_call_guards(void)
{
  TEST_BEGIN("c6link request guards");
  t_bringup();

  Rpc                   req;
  ra8_c6link_take_ctx_t take = {.link = &s_link, .out = nullptr, .rpc_id = 0U};
  rpc__init(&req);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_c6link_priv_rpc_call(nullptr, &req, 0U, ra8_c6link_priv_take_resp, &take));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_c6link_priv_rpc_call(&s_link, nullptr, 0U, ra8_c6link_priv_take_resp, &take));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_priv_rpc_call(&s_link, &req, 0U, nullptr, &take));

  /* A payload already staged means the transmit slot is taken, and this link
     carries exactly one. The second request is refused rather than silently
     serialised behind the first. */
  s_link.tx_len = 1U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_c6link_priv_rpc_call(&s_link, &req, 0U, ra8_c6link_priv_take_resp, &take));
  s_link.tx_len = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_c6link_priv_rpc_call(&s_link, &req, 0U, ra8_c6link_priv_take_resp, &take));

  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_priv_pump(nullptr, 1U, &stats));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_priv_pump(&s_link, 1U, nullptr));

  /* A request that cannot fit one frame is refused before anything is staged,
     rather than truncated onto the wire where the co-processor would have to
     reject it. Nothing this library sends is near the limit, so the case is
     constructed here. */
  t_bringup();
  RpcEventESPInit huge;
  rpc__event__espinit__init(&huge);
  huge.init_data.data = s_oversize;
  huge.init_data.len  = sizeof s_oversize;

  Rpc big;
  rpc__init(&big);
  big.msg_type       = RPC_TYPE__Req;
  big.msg_id         = RPC_ID__Req_GetCoprocessorFwVersion;
  big.payload_case   = RPC__PAYLOAD_EVENT_ESP_INIT;
  big.event_esp_init = &huge;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_c6link_priv_rpc_call(&s_link, &big, 0U, ra8_c6link_priv_take_resp, &take));
  TEST_END("c6link request guards");
}

/**
 * @par MC/DC:
 * Decision: `(link == nullptr) || (ev == nullptr)` (2 conditions,
 * ::ra8_c6link_priv_emit); `(link == nullptr) || (view == nullptr)`
 * (::ra8_c6link_priv_dispatch); `(link == nullptr) || (payload == nullptr)`
 * (::ra8_c6link_priv_rpc_consume); `(link == nullptr) || (out == nullptr)`
 * (::ra8_c6link_last_fault). Each is driven with the same three vectors:
 * - Vector 1: both supplied  -> false (control)
 * - Vector 2: first NULL     -> true  (varies the first condition only)
 * - Vector 3: second NULL    -> true  (varies the second condition only)
 * Decision: `(if_type == ESP_STA_IF) || (if_type == ESP_AP_IF)` (2 conditions,
 * ::ra8_c6link_priv_dispatch)
 * - Vector 4: a station frame       -> true  (varies the station test)
 * - Vector 5: an access-point frame -> true  (varies the AP test)
 * - Vector 6: a privileged frame    -> false (control: neither)
 * Decision: `armed || (tx_len != 0U)` (2 conditions, ::ra8_c6link_priv_rpc_call)
 * - Vector 7: neither         -> false (control: the request is issued)
 * - Vector 8: armed only      -> true  (varies armed only)
 * - Vector 9: staged only     -> true  (varies tx_len only)
 * Each control paired with each varied vector proves that condition
 * independently decides. N+1 vectors per decision: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_priv_emit
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_priv_dispatch
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_last_fault
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@ra8_c6link_priv_rpc_consume
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@ra8_c6link_priv_resp
 */
static void test_mcdc_facade_guards(void)
{
  TEST_BEGIN("c6link facade guard vectors");
  t_reset();

  /* Every private entry point tolerates either argument being absent. These
     are reachable from no public call, so they are driven directly. */
  const ra8_c6link_event_t ev         = {.kind = k_ra8_c6link_event_boot};
  ra8_c6link_rx_view_t     view       = {.offset  = (uint16_t)k_ra8_c6link_header_bytes,
                                         .len     = 1U,
                                         .if_type = (uint8_t)ESP_STA_IF};
  uint8_t                  payload[4] = {};

  ra8_c6link_priv_emit(&s_link, &ev);
  TEST_ASSERT_EQ(1, s_event_n);
  ra8_c6link_priv_emit(nullptr, &ev);
  ra8_c6link_priv_emit(&s_link, nullptr);
  TEST_ASSERT_EQ(1, s_event_n);

  TEST_ASSERT(!ra8_c6link_priv_dispatch(&s_link, &view));
  TEST_ASSERT(!ra8_c6link_priv_dispatch(nullptr, &view));
  TEST_ASSERT(!ra8_c6link_priv_dispatch(&s_link, nullptr));

  TEST_ASSERT(!ra8_c6link_priv_rpc_consume(&s_link, payload, (uint16_t)sizeof payload));
  TEST_ASSERT(!ra8_c6link_priv_rpc_consume(nullptr, payload, (uint16_t)sizeof payload));
  TEST_ASSERT(!ra8_c6link_priv_rpc_consume(&s_link, nullptr, (uint16_t)sizeof payload));

  ra8_c6link_fault_t fault = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(&s_link, &fault));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_last_fault(nullptr, &fault));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_last_fault(&s_link, nullptr));

  /* An access-point frame routes to the same receive callback a station frame
     does: the facade carries the data plane for both interfaces. */
  t_reset();
  uint8_t* slot = ra8_c6_model_slot();
  TEST_ASSERT_NOT_NULL(slot);
  ra8_c6link_priv_frame_seal(slot, (uint8_t)ESP_AP_IF, 0U, (uint16_t)k_c6m_eth_len);
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(&s_link, 2U, &stats));
  TEST_ASSERT_EQ(1, stats.eth_in);
  TEST_ASSERT_EQ(k_c6m_eth_len, s_rx_len);

  /* The busy test is two conditions: a request already outstanding, or a
     payload already staged. Either alone refuses. */
  t_reset();
  Rpc                   req;
  ra8_c6link_take_ctx_t take = {.link = &s_link, .out = nullptr, .rpc_id = 0U};
  rpc__init(&req);
  s_link.wait.armed = true;
  s_link.tx_len     = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_c6link_priv_rpc_call(&s_link, &req, 0U, ra8_c6link_priv_take_resp, &take));
  s_link.wait.armed = false;
  TEST_END("c6link facade guard vectors");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the announcement is transmitted, its
 * octets are checked against the protocol, and a request issued before it is
 * shown to go unanswered)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_frame.c@ra8_c6link_priv_caps
 */
static void test_host_announcement(void)
{
  TEST_BEGIN("c6link host announcement");

  /* Nothing is served before the host has announced itself. This is the
     regression test for a defect the host tests did not catch and the bench
     did: the facade answered once, against a co-processor a different
     application had already announced to, and never again. */
  t_reset();
  ra8_c6_model()->silent_boot = true;
  ra8_c6link_fw_version_t fw  = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(&s_link, &fw));

  /* Announce, and the same request is answered. */
  t_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_fw_version(&s_link, &fw));
  TEST_ASSERT_EQ(k_c6m_fw_major, fw.major);

  /* The octets are upstream's, tag for tag: an ESP_PRIV_EVENT_INIT header
     followed by five one-valued TLVs. */
  const ra8_c6_model_t* c6 = ra8_c6_model();
  TEST_ASSERT_EQ(k_c6m_caps_bytes, c6->caps_len);
  TEST_ASSERT_EQ(ESP_PRIV_EVENT_INIT, c6->caps[0]);
  TEST_ASSERT_EQ(k_t_caps_tlv_bytes, c6->caps[1]);
  TEST_ASSERT_EQ(HOST_CAPABILITIES, c6->caps[2]);
  TEST_ASSERT_EQ(1, c6->caps[3]);
  TEST_ASSERT_EQ(0, c6->caps[4]);
  TEST_ASSERT_EQ(RCVD_ESP_FIRMWARE_CHIP_ID, c6->caps[5]);
  TEST_ASSERT_EQ(k_c6m_chip_id, c6->caps[7]);
  TEST_ASSERT_EQ(SLV_CONFIG_TEST_RAW_TP, c6->caps[8]);
  TEST_ASSERT_EQ(SLV_CONFIG_THROTTLE_HIGH_THRESHOLD, c6->caps[11]);
  TEST_ASSERT_EQ(k_t_throttle_high, c6->caps[13]);
  TEST_ASSERT_EQ(SLV_CONFIG_THROTTLE_LOW_THRESHOLD, c6->caps[14]);
  TEST_ASSERT_EQ(k_t_throttle_low, c6->caps[16]);

  TEST_END("c6link host announcement");
}

int32_t main(void)
{
  test_open_validation();
  test_host_announcement();
  test_fw_version_roundtrip();
  test_answer_correlation();
  test_wifi_start_sequence();
  test_join_credentials();
  test_sta_cfg_set();
  test_mac_and_ap_info();
  test_events();
  test_events_remaining();
  test_join_pinned_bssid();
  test_eth_data_plane();
  test_transport_faults();
  test_undecodable();
  test_rejected_frames();
  test_rpc_call_guards();
  test_mcdc_facade_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_c6link.c\n");
  return 0;
}
