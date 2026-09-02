/**
 * @file test_ra8_c6link_session.c
 * @brief Session, Wi-Fi, and event facade tests against the C6 model.
 *
 * @details
 * These tests drive complete control-plane transactions through the bounded
 * co-processor model. Transport framing and rejection cases live in
 * `test_ra8_c6link_transport.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "idf_compat/esp_err.h"
#include "ra8_attributes.h"
#include "ra8_c6_model.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_c6link_model_test_internal.h"
#include "ra8_c6link_test_suites.h"
#include "ra8_c6link_wifi.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @enum t_c6_const_t @brief Constants owned by the session facade suite. */
typedef enum : uint32_t {
  k_t_ssid_len       = 9U,    /**< Length of the SSID the tests configure.       */
  k_t_junk_octet     = 0xFFU, /**< Filler used to prove credential erasure.      */
  k_t_pin_first      = 0xDEU, /**< First octet of the pinned BSSID a join sends. */
  k_t_pin_last       = 0xADU, /**< Its last octet.                               */
  k_t_pin_last_i     = 5U,    /**< Index of that last octet.                     */
  k_t_caps_tlv_bytes = 15U,   /**< Octets of TLV the announcement declares.      */
  k_t_throttle_high  = 80U,   /**< Flow-control high-water mark it advertises.   */
  k_t_throttle_low   = 60U,   /**< Flow-control low-water mark it advertises.    */
} t_c6_const_t;

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
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@internal_c6link_check_cfg
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_open
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_is_open
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_last_fault @brief Verify open validation behavior. @details Executes the open validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_open_validation(void)
{
  TEST_BEGIN("c6link open validation");
  ra8_c6_model_reset();
  ra8_c6link_t     link = {};
  ra8_c6link_cfg_t cfg  = {};
  priv_c6link_test_cfg(&cfg);
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
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@ra8_c6link_fw_version @brief Verify fw version roundtrip behavior. @details Executes the fw version roundtrip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_fw_version_roundtrip(void)
{
  TEST_BEGIN("c6link identity round trip");
  priv_c6link_test_bringup();
  ra8_c6link_fw_version_t fw = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));
  TEST_ASSERT_EQ(k_c6m_fw_major, fw.major);
  TEST_ASSERT_EQ(k_c6m_fw_minor, fw.minor);
  TEST_ASSERT_EQ(k_c6m_fw_patch, fw.patch);
  TEST_ASSERT_EQ(k_c6m_chip_id, fw.chip_id);
  TEST_ASSERT_EQ(0, strcmp(fw.target, "esp32c6"));
  TEST_ASSERT_EQ(1, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_GetCoprocessorFwVersion, ra8_c6_model()->seen[0]);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_fw_version(nullptr, &fw));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_fw_version(priv_c6link_test_link(), nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(priv_c6link_test_link()));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));
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
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@internal_c6link_rpc_answer
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@priv_c6link_rpc_consume @brief Verify answer correlation behavior. @details Executes the answer correlation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_answer_correlation(void)
{
  TEST_BEGIN("c6link answer correlation");
  ra8_c6link_fw_version_t fw = {};

  priv_c6link_test_bringup();
  ra8_c6_model()->wrong_uid = true;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));

  priv_c6link_test_bringup();
  ra8_c6_model()->wrong_id = true;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));

  priv_c6link_test_bringup();
  ra8_c6_model()->mute = true;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));

  /* An answer with no request outstanding must be counted and dropped, not
     acted on: the link is only polled here, never asked anything. */
  priv_c6link_test_bringup();
  ra8_c6_model_emit_stray();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 2U, &stats));
  TEST_ASSERT_EQ(1, stats.rpc_in);
  TEST_ASSERT_EQ(0, stats.events);
  TEST_END("c6link answer correlation");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the three start requests are observed in
 * order, and a scripted refusal of the middle one stops the sequence there) @brief Verify wifi start sequence behavior. @details Executes the wifi start sequence scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_wifi_start_sequence(void)
{
  TEST_BEGIN("c6link station start sequence");
  priv_c6link_test_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_start(priv_c6link_test_link()));
  TEST_ASSERT_EQ(3, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiInit, ra8_c6_model()->seen[0]);
  TEST_ASSERT_EQ(RPC_ID__Req_SetWifiMode, ra8_c6_model()->seen[1]);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiStart, ra8_c6_model()->seen[2]);

  ra8_c6link_fault_t fault = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(priv_c6link_test_link(), &fault));
  TEST_ASSERT_EQ(0, fault.rpc_id);

  priv_c6link_test_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_SetWifiMode;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_start(priv_c6link_test_link()));
  TEST_ASSERT_EQ(2, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_last_fault(priv_c6link_test_link(), &fault));
  TEST_ASSERT_EQ(RPC_ID__Req_SetWifiMode, fault.rpc_id);
  TEST_ASSERT_EQ(k_c6m_esp_fail, fault.resp);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_last_fault(priv_c6link_test_link(), nullptr));

  /* Teardown attempts both steps even when the first is refused: a
     co-processor left half-configured is worse than a reported fault. */
  priv_c6link_test_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiStop;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_stop(priv_c6link_test_link()));
  TEST_ASSERT_EQ(2, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiDeinit, ra8_c6_model()->seen[1]);

  priv_c6link_test_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_stop(priv_c6link_test_link()));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_start(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_stop(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_leave(nullptr));
  TEST_END("c6link station start sequence");
}

/**
 * @brief Assert that the complete C6 link transmit buffer is cleared.
 * @details Examines every byte rather than only the previously staged extent,
 *          proving failure paths do not retain credentials in unused storage.
 * @param[in] link Link fixture whose transmit buffer is inspected.
 * @return Nothing.
 * @pre `link` is non-null and belongs to the active test fixture.
 * @pre The credential-bearing RPC attempt has returned.
 * @post Every transmit-buffer byte was compared with zero.
 * @post The link fixture remains unchanged.
 * @note The loop is bounded by the fixed transmit-buffer size.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_tx_cleared(const ra8_c6link_t* link)
{
  for (size_t index = 0U; index < sizeof(link->tx); ++index) {
    TEST_ASSERT_EQ(0, link->tx[index]);
  }
}

/**
 * @brief Exercise credential-bearing join RPC failure paths.
 * @details Seeds the full transmit buffer before remote rejection, transport
 *          failure, and timeout scenarios, then proves every path erases it.
 * @param[in] sta Valid station configuration shared by the scenarios.
 * @return Nothing.
 * @pre `sta` is non-null and satisfies the join interface contract.
 * @pre The C6 link test model is available for repeated bring-up.
 * @post Every exercised transmit buffer is explicitly verified as zero.
 * @post Fixture mutations remain confined to the file-local test model.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_exercise_join_rpc_failures(const ra8_c6link_sta_cfg_t* sta)
{
  priv_c6link_test_bringup();
  ra8_c6link_t* link       = priv_c6link_test_link();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiSetConfig;
  (void)memset(link->tx, (int)k_t_junk_octet, sizeof(link->tx));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_join(link, sta));
  internal_assert_tx_cleared(link);

  priv_c6link_test_bringup();
  link                          = priv_c6link_test_link();
  ra8_c6_model()->fail_transfer = true;
  (void)memset(link->tx, (int)k_t_junk_octet, sizeof(link->tx));
  TEST_ASSERT_EQ(k_ra8_err_spi_error, ra8_c6link_wifi_join(link, sta));
  internal_assert_tx_cleared(link);

  priv_c6link_test_bringup();
  link                 = priv_c6link_test_link();
  ra8_c6_model()->mute = true;
  (void)memset(link->tx, (int)k_t_junk_octet, sizeof(link->tx));
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_wifi_join(link, sta));
  internal_assert_tx_cleared(link);
}

/**
 * @brief Exercise station-configuration validation before a join RPC.
 * @details Varies each bounded-length guard independently and covers both
 *          nullable public-interface arguments.
 * @param[in] sta Valid station configuration used as the control vector.
 * @return Nothing.
 * @pre `sta` is non-null and satisfies the join interface contract.
 * @pre The C6 link test fixture has been initialized.
 * @post Every invalid input is rejected with the documented error.
 * @post The control configuration remains unchanged.
 * @note File-local helper; no ownership escapes this test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_exercise_join_guards(const ra8_c6link_sta_cfg_t* sta)
{
  ra8_c6link_sta_cfg_t bad = *sta;
  bad.ssid_len             = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_wifi_join(priv_c6link_test_link(), &bad));
  bad          = *sta;
  bad.ssid_len = (uint8_t)k_ra8_c6link_ssid_max + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_wifi_join(priv_c6link_test_link(), &bad));
  bad          = *sta;
  bad.pass_len = (uint8_t)k_ra8_c6link_pass_max + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_c6link_wifi_join(priv_c6link_test_link(), &bad));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_join(priv_c6link_test_link(), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_join(nullptr, sta));
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
 * @brief Verify join credentials behavior.
 * @details Executes successful, failed-RPC, validation, and leave scenarios
 *          while checking that credential-bearing staging is erased.
 * @return Nothing.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre The C6 link test model is ready for repeated bring-up.
 * @post Every scenario produced its contract-specific result.
 * @post Credential-bearing transmit buffers were verified as zero.
 * @note File-local test; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_join_credentials(void)
{
  TEST_BEGIN("c6link credentials reach the co-processor");
  priv_c6link_test_bringup();
  ra8_c6link_sta_cfg_t sta = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_sta_cfg_set(&sta, "ra8-bench", "hunter2hunter2"));
  sta.channel = (uint8_t)k_c6m_channel;

  ra8_c6link_t* link = priv_c6link_test_link();
  (void)memset(link->tx, (int)k_t_junk_octet, sizeof(link->tx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_join(link, &sta));
  TEST_ASSERT_EQ(2, ra8_c6_model()->seen_n);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiSetConfig, ra8_c6_model()->seen[0]);
  TEST_ASSERT_EQ(RPC_ID__Req_WifiConnect, ra8_c6_model()->seen[1]);
  TEST_ASSERT_EQ(0, strcmp(ra8_c6_model()->ssid, "ra8-bench"));
  TEST_ASSERT_EQ(0, strcmp(ra8_c6_model()->pass, "hunter2hunter2"));
  internal_assert_tx_cleared(link);

  internal_exercise_join_rpc_failures(&sta);
  internal_exercise_join_guards(&sta);

  priv_c6link_test_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_leave(priv_c6link_test_link()));
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
 * Decisions: libs/ra8_c6link/src/ra8_c6link_wifi_sta.c@internal_c6link_sta_len @brief Verify sta cfg set behavior. @details Executes the sta cfg set scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_sta_cfg_set(void)
{
  TEST_BEGIN("c6link station configuration builder");
  ra8_c6link_sta_cfg_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_sta_cfg_set(nullptr, "x", nullptr));
  (void)memset(&cfg, (int)k_t_junk_octet, sizeof(cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_sta_cfg_set(&cfg, nullptr, nullptr));
  const uint8_t* cfg_bytes = (const uint8_t*)&cfg;
  for (size_t index = 0U; index < sizeof(cfg); ++index) {
    TEST_ASSERT_EQ(0, cfg_bytes[index]);
  }

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
 * Decisions: libs/ra8_c6link/src/ra8_c6link_wifi_sta.c@ra8_c6link_wifi_ap_info @brief Verify mac and ap info behavior. @details Executes the mac and ap info scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mac_and_ap_info(void)
{
  TEST_BEGIN("c6link station address and AP record");
  const uint8_t want[] = {9U, 8U, 7U, 6U, 5U, 4U};

  priv_c6link_test_bringup();
  ra8_c6link_mac_t mac = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_mac(priv_c6link_test_link(), &mac));
  TEST_ASSERT_EQ(0, memcmp(mac.octet, want, sizeof want));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_mac(priv_c6link_test_link(), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_mac(nullptr, &mac));

  priv_c6link_test_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_GetMACAddress;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_mac(priv_c6link_test_link(), &mac));

  priv_c6link_test_bringup();
  ra8_c6link_ap_info_t ap = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_ap_info(priv_c6link_test_link(), &ap));
  TEST_ASSERT_EQ(0, strcmp(ap.ssid, "benc"));
  TEST_ASSERT_EQ(k_c6m_channel, ap.channel);
  TEST_ASSERT_EQ(-(int32_t)k_c6m_rssi_mag, ap.rssi);
  TEST_ASSERT_EQ(0, memcmp(ap.bssid.octet, want, sizeof want));

  /* An unassociated station is what a refusal here means, and it must surface
     as a protocol error carrying the co-processor's own code. */
  priv_c6link_test_bringup();
  ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_WifiStaGetApInfo;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_c6link_wifi_ap_info(priv_c6link_test_link(), &ap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_ap_info(nullptr, &ap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_wifi_ap_info(priv_c6link_test_link(), nullptr));
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
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@priv_c6link_emit @brief Verify events behavior. @details Executes the events scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_events(void)
{
  TEST_BEGIN("c6link announcements");
  priv_c6link_test_reset();
  ra8_c6link_fw_version_t fw = {};
  /* The announcement is what provokes the boot event; the model answers it the
     way the silicon does rather than the test queueing one by hand. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_await_ready(priv_c6link_test_link(),
                                        (uint16_t)k_ra8_c6link_announce_transfers,
                                        &fw));
  TEST_ASSERT(ra8_c6_model()->caps_seen);
  TEST_ASSERT_EQ(1, priv_c6link_test_event_count());
  TEST_ASSERT_EQ(k_ra8_c6link_event_boot, priv_c6link_test_event(0U)->kind);

  ra8_c6_model_emit_disconnected();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 4U, &stats));
  TEST_ASSERT_EQ(2, priv_c6link_test_event_count());
  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_disconnected, priv_c6link_test_event(1U)->kind);
  TEST_ASSERT_EQ(k_c6m_reason, priv_c6link_test_event(1U)->reason);
  TEST_ASSERT_EQ(-(int32_t)k_c6m_rssi_mag, priv_c6link_test_event(1U)->rssi);
  TEST_ASSERT_EQ(0, strcmp(priv_c6link_test_event(1U)->ssid, "benc"));

  /* THE REGRESSION THIS ENTRY POINT EXISTS FOR. `Event_ESPInit` fires once,
     when the CO-PROCESSOR boots. The C6 on the bench has its own supply, so
     resetting this host does not reboot it and that event is long gone -- it
     was consumed by whichever application was clocking the bus when it fired.
     A link that treated the event as its readiness signal worked exactly once,
     on a freshly-flashed co-processor, and timed out on every run after. With
     the model emitting no boot event at all, readiness must still be
     established and the identity must still come back. */
  priv_c6link_test_reset();
  ra8_c6_model()->silent_boot   = true;
  ra8_c6link_fw_version_t quiet = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_c6link_await_ready(priv_c6link_test_link(),
                                        (uint16_t)k_ra8_c6link_announce_transfers,
                                        &quiet));
  TEST_ASSERT_EQ(0, priv_c6link_test_event_count());
  TEST_ASSERT_EQ(k_c6m_chip_id, quiet.chip_id);
  TEST_ASSERT(ra8_c6_model()->caps_seen);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_await_ready(nullptr, 2U, &quiet));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_await_ready(priv_c6link_test_link(), 2U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_c6link_await_ready(priv_c6link_test_link(), 0U, &quiet));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(priv_c6link_test_link()));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_c6link_await_ready(priv_c6link_test_link(), 2U, &quiet));
  TEST_END("c6link announcements");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the two remaining announcement kinds are
 * decoded from real protobuf events and compared field by field)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_rpc.c@internal_c6link_rpc_event @brief Verify events remaining behavior. @details Executes the events remaining scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_events_remaining(void)
{
  TEST_BEGIN("c6link association announcements");
  const uint8_t want_bssid[] = {2U, 3U, 4U, 5U, 6U, 7U};

  priv_c6link_test_reset();
  ra8_c6_model_emit_connected();
  ra8_c6_model_emit_wifi_event();
  ra8_c6link_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_poll(priv_c6link_test_link(), 4U, &stats));
  TEST_ASSERT_EQ(2, priv_c6link_test_event_count());
  TEST_ASSERT_EQ(2, stats.events);

  TEST_ASSERT_EQ(k_ra8_c6link_event_sta_connected, priv_c6link_test_event(0U)->kind);
  TEST_ASSERT_EQ(0, strcmp(priv_c6link_test_event(0U)->ssid, "benc"));
  TEST_ASSERT_EQ(k_c6m_channel, priv_c6link_test_event(0U)->channel);
  TEST_ASSERT_EQ(0, memcmp(priv_c6link_test_event(0U)->bssid.octet, want_bssid, sizeof want_bssid));

  /* The bare Wi-Fi event is the shape the co-processor raises most often: the
     bench run saw WIFI_EVENT_STA_START and WIFI_EVENT_STA_STOP arrive this
     way, carrying nothing but their own id. */
  TEST_ASSERT_EQ(k_ra8_c6link_event_wifi, priv_c6link_test_event(1U)->kind);
  TEST_ASSERT_EQ(k_c6m_wifi_ev, priv_c6link_test_event(1U)->wifi_event_id);
  TEST_ASSERT_EQ(0, priv_c6link_test_event(1U)->ssid_len);
  TEST_END("c6link association announcements");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- a pinned BSSID reaches the co-processor
 * as a six-octet field, and a request id that is not a bare one is refused) @brief Verify join pinned bssid behavior. @details Executes the join pinned bssid scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_join_pinned_bssid(void)
{
  TEST_BEGIN("c6link pinned BSSID and unknown bare request");
  priv_c6link_test_bringup();
  ra8_c6link_sta_cfg_t sta = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_sta_cfg_set(&sta, "ra8-bench", "hunter2hunter2"));
  sta.bssid_set                   = true;
  sta.bssid.octet[0]              = (uint8_t)k_t_pin_first;
  sta.bssid.octet[k_t_pin_last_i] = (uint8_t)k_t_pin_last;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_wifi_join(priv_c6link_test_link(), &sta));
  TEST_ASSERT_EQ(RPC_ID__Req_WifiSetConfig, ra8_c6_model()->seen[0]);

  /* A request id this helper does not know is refused rather than encoded as
     an empty message the co-processor would have to guess at. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 priv_c6link_bare_req(priv_c6link_test_link(), (uint32_t)RPC_ID__Req_WifiInit));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_c6link_bare_req(nullptr, 0U));
  TEST_END("c6link pinned BSSID and unknown bare request");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the announcement is transmitted, its
 * octets are checked against the protocol, and a request issued before it is
 * shown to go unanswered)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_frame.c@priv_c6link_caps @brief Verify host announcement behavior. @details Executes the host announcement scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_host_announcement(void)
{
  TEST_BEGIN("c6link host announcement");

  /* Nothing is served before the host has announced itself. This is the
     regression test for a defect the host tests did not catch and the bench
     did: the facade answered once, against a co-processor a different
     application had already announced to, and never again. */
  priv_c6link_test_reset();
  ra8_c6_model()->silent_boot = true;
  ra8_c6link_fw_version_t fw  = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));

  /* Announce, and the same request is answered. */
  priv_c6link_test_bringup();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_fw_version(priv_c6link_test_link(), &fw));
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

/**
 * @brief Run the session, Wi-Fi, and event facade scenarios.
 * @return Nothing.
 * @pre The bounded C6 model fixture is linked into this test executable.
 * @post Every session-facing facade scenario has completed its assertions.
 * @since 0.1.0
 */
void ra8_test_c6link_session(void)
{
  internal_test_open_validation();
  internal_test_host_announcement();
  internal_test_fw_version_roundtrip();
  internal_test_answer_correlation();
  internal_test_wifi_start_sequence();
  internal_test_join_credentials();
  internal_test_sta_cfg_set();
  internal_test_mac_and_ap_info();
  internal_test_events();
  internal_test_events_remaining();
  internal_test_join_pinned_bssid();
}
