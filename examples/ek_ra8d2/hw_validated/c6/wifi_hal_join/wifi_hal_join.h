/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie */
/**
 * @file examples/ek_ra8d2/hw_validated/c6/wifi_hal_join/wifi_hal_join.h
 * @brief Constants, console formatters and the DHCP provider for the HAL join.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The companion to ``main.c``. It holds the build constants, the bounded
 * console serialisers (so the image needs no ``printf`` and no heap), and the
 * one function that is genuinely stack-specific: ::wifi_hal_ip_bind, the
 * ::ra8_wifi_ip_bind_fn that runs a NetX Duo DHCP client to a lease. Everything
 * Wi-Fi lives behind ``ra8_wifi.h`` -- this header adds only what a bench
 * application needs around it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_wifi.h"

/**
 * @enum wifi_hal_cfg_t
 * @brief Clocking, pacing and RAM budgets the application sets.
 *
 * @details
 * The SPI bit rate and boot wait are the bench-proven figures the c6 examples
 * settled on. The arena and worker stack size the one link and the one worker
 * thread this image owns.
 *
 * @invariant ::k_wifi_hal_arena_bytes is at least ::k_ra8_c6link_arena_min.
 * @invariant ::k_wifi_hal_sck_hz is the rate silicon ran the link at.
 *
 * @par Example:
 * @code
 * const uint32_t sck = (uint32_t)k_wifi_hal_sck_hz;
 * @endcode
 *
 * @see ra8_wifi_init
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wifi_hal_uart_baud    = 115200U,  /**< Console rate over the J-Link OB VCOM.  */
  k_wifi_hal_sck_hz       = 5000000U, /**< SPI bit rate; the rate silicon ran at. */
  k_wifi_hal_edge_poll_ms = 2U,       /**< Poll period for a side-band pin.       */
  k_wifi_hal_boot_wait_ms = 200U,     /**< Settling delay before the first frame. */
  k_wifi_hal_heartbeat_ms = 5000U,    /**< Heartbeat gap after the verdict.       */
  k_wifi_hal_worker_stack = 8192U,    /**< Worker-thread stack, in bytes.         */
  k_wifi_hal_worker_prio  = 8U,       /**< Worker priority and preemption cap.    */
  k_wifi_hal_arena_bytes  = 4096U,    /**< Decode arena handed to the facade.     */
} wifi_hal_cfg_t;

/**
 * @enum wifi_hal_wait_t
 * @brief How long the application waits at each blocking step.
 *
 * @details
 * ::ra8_wifi_connect blocks internally, but a slow association may outlast its
 * budget, so the application then pumps ::ra8_wifi_poll a bounded number of
 * times. The DHCP wait is the NetX lease budget.
 *
 * @invariant ::k_wifi_hal_assoc_polls is non-zero, so at least one poll runs.
 * @invariant ::k_wifi_hal_dhcp_wait_ms comfortably exceeds one DHCP exchange.
 *
 * @par Example:
 * @code
 * for (uint32_t i = 0U; i < (uint32_t)k_wifi_hal_assoc_polls; i++) { ... }
 * @endcode
 *
 * @see ra8_wifi_poll
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wifi_hal_assoc_polls  = 200U,   /**< Extra association polls after connect. */
  k_wifi_hal_assoc_gap_ms = 50U,    /**< Gap between association polls.         */
  k_wifi_hal_dhcp_wait_ms = 25000U, /**< DHCP lease wait budget, ticks.         */
} wifi_hal_wait_t;

/**
 * @enum wifi_hal_fmt_t
 * @brief Widths the bounded console formatters work to.
 *
 * @details
 * Restated locally so the console loops have named bounds (NASA Rule 2) without
 * reaching into another example's header.
 *
 * @invariant ::k_wifi_hal_dec_digits holds the widest 32-bit decimal value.
 * @invariant ::k_wifi_hal_ip_octets is four, the octet count of an IPv4 address.
 *
 * @par Example:
 * @code
 * wifi_hal_put_hex(chip_id, (uint8_t)k_wifi_hal_hex_byte);
 * @endcode
 *
 * @see wifi_hal_put_u32
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_wifi_hal_str_max    = 256U,  /**< Longest string the console helper emits.   */
  k_wifi_hal_dec_radix  = 10U,   /**< Decimal radix.                             */
  k_wifi_hal_dec_digits = 10U,   /**< Digits in the widest 32-bit decimal value. */
  k_wifi_hal_hex_digits = 8U,    /**< Digits in the widest 32-bit hex value.     */
  k_wifi_hal_hex_bits   = 4U,    /**< Bits per hexadecimal digit.                */
  k_wifi_hal_hex_mask   = 0x0FU, /**< Nibble mask.                               */
  k_wifi_hal_hex_alpha  = 10U,   /**< First nibble value spelled with a letter.  */
  k_wifi_hal_hex_byte   = 2U,    /**< Hex digits printed for a byte-wide field.  */
  k_wifi_hal_ip_octets  = 4U,    /**< Octets in an IPv4 address.                 */
  k_wifi_hal_ip_mask    = 0xFFU, /**< Single-octet mask for IPv4 formatting.     */
} wifi_hal_fmt_t;

/**
 * @enum wifi_hal_ip_shift_t
 * @brief Bit shifts that pick each octet out of a packed IPv4 address.
 * @details Separated from ::wifi_hal_fmt_t only to keep an 8-bit shift set in
 *          its own smallest type.
 * @invariant The four shifts are 24, 16, 8 and 0, high octet first.
 * @invariant Each shift is a multiple of eight.
 * @par Example:
 * @code
 * const uint8_t o0 = (uint8_t)((ip >> k_wifi_hal_ip_shift_0) & k_wifi_hal_ip_mask);
 * @endcode
 * @see wifi_hal_put_ip
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_wifi_hal_ip_shift_0 = 24U, /**< Shift for the first (highest) octet. */
  k_wifi_hal_ip_shift_1 = 16U, /**< Shift for the second octet.          */
  k_wifi_hal_ip_shift_2 = 8U,  /**< Shift for the third octet.           */
  k_wifi_hal_ip_shift_3 = 0U,  /**< Shift for the fourth (lowest) octet. */
} wifi_hal_ip_shift_t;

/**
 * @var k_wifi_hal_pass_line
 * @brief The exact PASS line the run prints; the HIL gate keys on it.
 * @details Shared by the core (which selects it) and ``main.c`` (which prints
 *          it), so the string the host test asserts is the string that reaches
 *          the console on silicon. ``hil.conf``'s HIL_EXPECT is a substring of
 *          it.
 * @since 0.1.0
 */
extern const char k_wifi_hal_pass_line[];

/**
 * @var k_wifi_hal_fail_line
 * @brief The summary FAIL line the run prints when it did not reach an IP.
 * @details The counterpart to ::k_wifi_hal_pass_line; specific failures also
 *          print a diagnostic sub-line that HIL_EXPECT_NEGATIVE matches.
 * @since 0.1.0
 */
extern const char k_wifi_hal_fail_line[];

/**
 * @struct wifi_hal_result
 * @brief The observable outcome of one ::wifi_hal_join_run, for the banner and
 *        the host test.
 * @details Populated by the core. ``main.c`` prints these fields; the host test
 *          asserts them. Every field is a fact the run established, not a
 *          formatting artifact.
 * @invariant `passed` is true exactly when `ip_bound` is, i.e. the whole
 *            journey completed.
 * @invariant `associated` is set before `ip_bound` can be.
 * @invariant Each `*_err` is the verdict of the call it names, or
 *            ::k_ra8_err_not_initialized where the run stopped before that call
 *            was reached -- so a failure always names a step and a reason
 *            rather than only "init=fail".
 * @par Example:
 * @code
 * wifi_hal_result_t res = {};
 * (void)wifi_hal_join_run(&cfg, &res);
 * @endcode
 * @see wifi_hal_join_run
 * @since 0.1.0
 */
typedef struct wifi_hal_result {
  bool             init_ok;     /**< The facade initialised.           */
  bool             associated;  /**< The station joined the network.   */
  bool             ip_bound;    /**< A DHCP lease was obtained.        */
  bool             passed;      /**< The whole journey completed.      */
  ra8_err_t        init_err;    /**< What ::ra8_wifi_init returned.    */
  ra8_err_t        connect_err; /**< What ::ra8_wifi_connect returned. */
  ra8_err_t        ip_err;      /**< What ::ra8_wifi_wait_ip returned. */
  ra8_wifi_mac_t   mac;         /**< Station MAC, once associated.     */
  ra8_wifi_ap_t    ap;          /**< AP record, once associated.       */
  ra8_wifi_lease_t lease;       /**< The DHCP lease, once bound.       */
} wifi_hal_result_t;

/**
 * @struct wifi_hal_run_cfg
 * @brief Injected dependencies for the hardware-free ::wifi_hal_join_run.
 * @details The facade configuration and handle are caller-owned, so the same
 *          core runs in two places: ``main.c`` builds the config over the C6
 *          backend and a NetX DHCP provider, and the host test builds it over a
 *          mock backend and a canned provider. The core touches no hardware, no
 *          RTOS and no console.
 * @invariant `wifi_cfg` and `wifi` are non-null and out-live the run.
 * @invariant `poll_budget` is non-zero, so the association wait runs at least
 *            once.
 * @par Example:
 * @code
 * wifi_hal_run_cfg_t cfg = { .wifi = &s_wifi, .wifi_cfg = &wcfg,
 *                            .ssid = s_ssid, .psk = s_psk,
 *                            .poll_budget = (uint32_t)k_wifi_hal_assoc_polls };
 * @endcode
 * @see wifi_hal_join_run
 * @since 0.1.0
 */
typedef struct wifi_hal_run_cfg {
  const ra8_wifi_cfg_t* wifi_cfg;    /**< Facade config: backend + ip provider.    */
  ra8_wifi_t*           wifi;        /**< Caller-allocated facade handle.          */
  const char*           ssid;        /**< Target SSID; empty means "no creds".     */
  const char*           psk;         /**< Passphrase, or null for an open network. */
  uint32_t              poll_budget; /**< Association poll attempts after connect. */
} wifi_hal_run_cfg_t;

/**
 * @brief The example's whole network journey, hardware-free: init, join, DHCP.
 *
 * @details
 * The wiring this example demonstrates, extracted so it is testable on the host
 * exactly as it runs on the board. It initialises the facade, checks that
 * credentials were compiled in, connects, settles the association, reads the MAC
 * and AP record, and obtains an IP -- recording each fact in @p out and stopping
 * at the first step that fails. It performs no I/O of its own; the caller prints
 * from @p out.
 *
 * @param[in] cfg Injected dependencies; must be non-null with a facade config
 *                and handle.
 * @param[out] out Result to populate; must be non-null.
 *
 * @return true when the journey reached a bound IP (@p out->passed).
 * @retval true The station associated and DHCP leased an address.
 * @retval false A step failed; @p out says how far it got.
 *
 * @pre The facade config's backend hardware (or mock) is ready.
 * @pre @p cfg->ssid is NUL-terminated.
 * @post @p out is fully populated, including on every failure path.
 * @post On success @p out->ip_bound and @p out->passed are true.
 *
 * @note Not thread-safe; it drives the facade. Touches no hardware, RTOS or
 *       console, so it runs unchanged in a host test.
 *
 * @par Example:
 * @code
 * wifi_hal_result_t res = {};
 * if (wifi_hal_join_run(&cfg, &res)) { announce(res.lease.ip); }
 * @endcode
 *
 * @see wifi_hal_result
 * @since 0.1.0
 */
[[nodiscard]] bool wifi_hal_join_run(const wifi_hal_run_cfg_t* cfg, wifi_hal_result_t* out);

/**
 * @brief The application's IP provider: a NetX Duo DHCP client to a lease.
 *
 * @details
 * The ::ra8_wifi_ip_bind_fn ::ra8_wifi_wait_ip calls. It binds the wireless
 * NetX link driver to the open C6 link, creates the packet pool and IP
 * instance, runs the vendored DHCP client to a bound lease, and copies the
 * lease out. From here NetX Duo owns the wire.
 *
 * @param[in] ip_ctx The open ``ra8_c6link_t*`` this application passed as
 *                   ::ra8_wifi_cfg::ip_ctx; must be non-null.
 * @param[in] mac The station MAC the IP stack must adopt; must be non-null.
 * @param[out] out Lease to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok A lease was obtained and copied into @p out.
 * @retval k_ra8_err_null_ptr An argument was null.
 * @retval k_ra8_err_not_initialized The NetX objects could not be created.
 * @retval k_ra8_err_timeout DHCP did not bind within ::k_wifi_hal_dhcp_wait_ms.
 *
 * @pre The station is associated (::ra8_wifi_connect has succeeded).
 * @pre @p ip_ctx is the same open link the facade drives.
 * @post On success @p out->bound is implied by a non-zero @p out->ip.
 * @post On failure @p out is cleared.
 *
 * @note Runs once, on the worker thread, inside ::ra8_wifi_wait_ip.
 * @warning Blocks up to ::k_wifi_hal_dhcp_wait_ms waiting for the lease.
 *
 * @par Example:
 * @code
 * cfg.ip_bind = wifi_hal_ip_bind;
 * cfg.ip_ctx  = &s_link;
 * @endcode
 *
 * @see ra8_wifi_wait_ip
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
wifi_hal_ip_bind(void* ip_ctx, const ra8_wifi_mac_t* mac, ra8_wifi_lease_t* out);

/**
 * @brief Write a NUL-terminated string to the console, bounded.
 * @param[in] text String to write, or null for a no-op; at most
 *                 ::k_wifi_hal_str_max octets are emitted.
 * @return Nothing.
 * @pre @p text is NUL-terminated within ::k_wifi_hal_str_max octets.
 * @pre The console has been initialised.
 * @post At most ::k_wifi_hal_str_max octets were written.
 * @post No state beyond the UART is touched.
 * @note Not thread-safe; call from one context.
 * @see wifi_hal_put_u32
 * @since 0.1.0
 */
void wifi_hal_puts(const char* text);

/**
 * @brief Write an unsigned 32-bit value in decimal.
 * @param[in] value Value to write.
 * @return Nothing.
 * @pre The console has been initialised.
 * @pre The caller accepts base-ten output only.
 * @post Between one and ::k_wifi_hal_dec_digits characters were emitted.
 * @post No state beyond the UART is touched.
 * @note Not thread-safe.
 * @see wifi_hal_put_hex
 * @since 0.1.0
 */
void wifi_hal_put_u32(uint32_t value);

/**
 * @brief Write the low @p digits nibbles of a value in hexadecimal.
 * @param[in] value Value whose low nibbles are printed.
 * @param[in] digits Hex digits to print; 1..::k_wifi_hal_hex_digits.
 * @return Nothing.
 * @pre @p digits is in range, else the call is a no-op.
 * @pre The console has been initialised.
 * @post Exactly @p digits characters were emitted on the valid path.
 * @post No state beyond the UART is touched.
 * @note Not thread-safe.
 * @see wifi_hal_put_u32
 * @since 0.1.0
 */
void wifi_hal_put_hex(uint32_t value, uint8_t digits);

/**
 * @brief Write a packed IPv4 address as a dotted quad.
 * @param[in] ip Address in host order, high octet in the most significant byte.
 * @return Nothing.
 * @pre The console has been initialised.
 * @pre @p ip is host-order packed, as NetX returns it.
 * @post Four octets separated by dots were emitted.
 * @post No state beyond the UART is touched.
 * @note Not thread-safe.
 * @see wifi_hal_put_u32
 * @since 0.1.0
 */
void wifi_hal_put_ip(uint32_t ip);

/**
 * @brief Write a station MAC as colon-separated hex octets.
 * @param[in] mac Address to print, or null for a no-op.
 * @return Nothing.
 * @pre @p mac, when non-null, holds ::k_ra8_wifi_mac_bytes octets.
 * @pre The console has been initialised.
 * @post Six octets separated by colons were emitted on the valid path.
 * @post No state beyond the UART is touched.
 * @note Not thread-safe.
 * @see wifi_hal_put_hex
 * @since 0.1.0
 */
void wifi_hal_put_mac(const ra8_wifi_mac_t* mac);

/**
 * @brief Print the start-up banner naming the clocks and the link geometry.
 * @param[in] cpuclk_hz CPUCLK0 rate in hertz.
 * @param[in] pclka_hz PCLKA rate in hertz.
 * @return Nothing.
 * @pre The console has been initialised.
 * @pre Both rates are the live cached values.
 * @post One multi-line banner was emitted.
 * @post No state beyond the UART is touched.
 * @note Not thread-safe.
 * @see wifi_hal_puts
 * @since 0.1.0
 */
void wifi_hal_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz);
