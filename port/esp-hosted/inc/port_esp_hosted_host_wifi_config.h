/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/inc/port_esp_hosted_host_wifi_config.h
 * @brief esp-hosted port header: which Wi-Fi and supplicant features exist.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * One of the ten port headers the vendored esp-hosted core includes by name
 * (see ``docs/SOUP/esp-hosted-host.md``); ten vendored translation units --
 * the whole RPC and public-API layer -- reach it. Upstream generates its
 * equivalent from Kconfig under ``host/port/``, which this project excludes;
 * this file is its first-party replacement and keeps the upstream file name
 * and every upstream macro spelling exactly.
 *
 * @par These flags describe the co-processor, not a preference
 * Every switch here answers one of two factual questions:
 *   - **What can the radio do?** The co-processor is an ESP32-C6: a
 *     single-band 2.4 GHz part with 802.11ax (Wi-Fi 6) support. Whatever the
 *     host asks for beyond that, the radio cannot deliver.
 *   - **What is in the co-processor image?** It is built from ESP-IDF
 *     v5.5.4 with the defaults in ``coprocessor/esp32c6/sdkconfig.defaults``
 *     (``coprocessor/esp32c6/pins.env`` pins both versions). A structure
 *     field or an RPC identifier exists on that image or it does not, and a
 *     flag that claims otherwise makes the host serialise a request the
 *     co-processor will reject -- or, worse, decode a response field that is
 *     not there.
 * That is why none of the reasons below is "upstream default": the default
 * is only relevant as evidence about what v5.5.4 actually compiled.
 *
 * @par Version-shaped flags
 * ``H_PRESENT_IN_ESP_IDF_5_4_0`` and ``H_PRESENT_IN_ESP_IDF_5_5_0`` gate
 * individual structure fields that IDF added in those releases (the 5 GHz
 * RSSI adjustment, the SAE extension, the VHT beamformee bits). v5.5.4 is at
 * or past both, so both are on and the host mirrors the full structure.
 *
 * @since 0.1.0
 */

#pragma once

/* ----------------------------------------------------------------------- */
/* Radio capability */
/* ----------------------------------------------------------------------- */

/**
 * @def H_WIFI_HE_SUPPORT
 * @brief Whether the co-processor's radio speaks 802.11ax (Wi-Fi 6).
 * @details One. The ESP32-C6 is an 802.11ax part, so the high-efficiency
 * configuration blocks -- the HE bitmask in the station config, the TWT
 * calls, the HE-aware scan record -- are all meaningful on this link.
 * @note Read-only build configuration.
 * @warning Turning this off does not merely hide an option: the host stops
 *          serialising the HE half of the station configuration, and the
 *          co-processor then joins with defaults the application never
 *          chose.
 * @par Example:
 * @code
 * #if H_WIFI_HE_SUPPORT
 * p_c_sta->he_bitmask = encode_he_bits(cfg);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_HE_SUPPORT (1)

/**
 * @def H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
 * @brief Whether the HE structures use the post-5.3 layout.
 * @details One. The co-processor image is ESP-IDF v5.5.4, comfortably past
 * v5.3, where the high-efficiency fields moved into the bitmask form the
 * vendored encoder writes.
 * @note Read-only build configuration.
 * @warning Must track the image's IDF version, not the host's ambitions;
 *          the wrong layout mis-decodes every station configuration.
 * @par Example:
 * @code
 * #if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
 * use_he_bitmask_layout();
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3 (1)

/**
 * @def H_WIFI_DUALBAND_SUPPORT
 * @brief Whether the co-processor's radio covers 5 GHz as well as 2.4 GHz.
 * @details Zero. The ESP32-C6 is single-band 2.4 GHz; there is no 5 GHz PHY
 * on the part, so band selection and the dual-band scan variants have
 * nothing to drive.
 * @note Read-only build configuration.
 * @warning Setting this to 1 makes the host offer a band the radio does not
 *          have, and the resulting failures surface as scan timeouts rather
 *          than as a clear rejection.
 * @par Example:
 * @code
 * #if H_WIFI_DUALBAND_SUPPORT
 * esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_DUALBAND_SUPPORT (0)

/* ----------------------------------------------------------------------- */
/* Supplicant features present in the co-processor image */
/* ----------------------------------------------------------------------- */

/**
 * @def H_WIFI_ENTERPRISE_SUPPORT
 * @brief Whether the image carries the WPA2/WPA3-Enterprise supplicant.
 * @details One. ESP-IDF builds the enterprise supplicant into the C6 Wi-Fi
 * image by default, and ``sdkconfig.defaults`` does not disable it, so the
 * EAP client and its RPC surface are there to be driven.
 * @note Read-only build configuration.
 * @warning This switch also controls whether ``esp_eap_client.h`` is
 *          included by the RPC headers; turning it off removes types the
 *          public API declares.
 * @par Example:
 * @code
 * #if H_WIFI_ENTERPRISE_SUPPORT
 * #include "esp_eap_client.h"
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_ENTERPRISE_SUPPORT (1)

/**
 * @def H_GOT_SET_EAP_METHODS_API
 * @brief Whether the image exposes the "set EAP methods" call.
 * @details One. ``esp_eap_client_set_eap_methods()`` is part of the EAP
 * client in ESP-IDF v5.5.4, so both the RPC request encoder and the public
 * ``esp_eap_client_remote_set_eap_methods()`` prototype are real.
 * @note Read-only build configuration.
 * @warning Only meaningful with ::H_WIFI_ENTERPRISE_SUPPORT on; the two are
 *          nested upstream and an inconsistent pair drops the prototype
 *          while keeping its RPC case.
 * @par Example:
 * @code
 * #if H_GOT_SET_EAP_METHODS_API
 * case RPC_ID__Req_EapSetEapMethods: encode_methods(); break;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_GOT_SET_EAP_METHODS_API (1)

/**
 * @def H_GOT_EAP_OKC_SUPPORT
 * @brief Whether the image exposes opportunistic key caching control.
 * @details One. The OKC toggle is in the v5.5.4 EAP client, so the host can
 * offer the matching RPC request.
 * @note Read-only build configuration.
 * @warning A 1 here on an older co-processor image produces an RPC id the
 *          far end does not recognise, which fails the whole transaction.
 * @par Example:
 * @code
 * #if H_GOT_EAP_OKC_SUPPORT
 * case RPC_ID__Req_WifiSetOkcSupport: encode_okc(); break;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_GOT_EAP_OKC_SUPPORT (1)

/**
 * @def H_GOT_EAP_SET_DOMAIN_NAME
 * @brief Whether the image exposes the EAP domain-name setter.
 * @details One; present in the v5.5.4 EAP client alongside the OKC control.
 * @note Read-only build configuration.
 * @warning See ::H_GOT_EAP_OKC_SUPPORT for what an over-claimed RPC id does.
 * @par Example:
 * @code
 * #if H_GOT_EAP_SET_DOMAIN_NAME
 * case RPC_ID__Req_EapSetDomainName: encode_domain(); break;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_GOT_EAP_SET_DOMAIN_NAME (1)

/**
 * @def H_GOT_AP_CONFIG_PARAM_TRANSITION_DISABLE
 * @brief Whether the AP configuration carries the transition-disable bit.
 * @details One. ``transition_disable`` -- the WPA3 downgrade-protection
 * indication an access point advertises -- is a field of the v5.5.4 AP
 * configuration structure, so it is copied in both RPC directions.
 * @note Read-only build configuration.
 * @warning The field is copied field-by-field, not memcpy'd, so a 1 here
 *          against a structure without it is a compile error rather than
 *          silent corruption.
 * @par Example:
 * @code
 * #if H_GOT_AP_CONFIG_PARAM_TRANSITION_DISABLE
 * p_c_ap->transition_disable = p_a_ap->transition_disable;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_GOT_AP_CONFIG_PARAM_TRANSITION_DISABLE (1)

/**
 * @def H_GOT_TWT_ENABLE_KEEP_ALIVE
 * @brief Whether the TWT configuration carries the keep-alive flag.
 * @details One. Target Wake Time is part of 802.11ax, the C6 supports it,
 * and ``twt_enable_keep_alive`` is a field of the v5.5.4 TWT configuration.
 * @note Read-only build configuration.
 * @warning Only reachable while ::H_WIFI_HE_SUPPORT is 1; TWT is an HE
 *          feature.
 * @par Example:
 * @code
 * #if H_GOT_TWT_ENABLE_KEEP_ALIVE
 * req->config->twt_enable_keep_alive = cfg.twt_enable_keep_alive;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_GOT_TWT_ENABLE_KEEP_ALIVE (1)

/* ----------------------------------------------------------------------- */
/* ESP-IDF structure-version gates */
/* ----------------------------------------------------------------------- */

/**
 * @def H_PRESENT_IN_ESP_IDF_5_4_0
 * @brief Whether fields added in ESP-IDF v5.4.0 are present.
 * @details One; the co-processor image is v5.5.4. The field this actually
 * gates is the scan threshold's 5 GHz RSSI adjustment, which exists in the
 * structure even on a single-band part and must therefore be copied for the
 * two ends to agree on the layout.
 * @note Read-only build configuration.
 * @warning Structure layout, not capability: leaving it off on a v5.5.4
 *          image would silently drop a field the co-processor still expects.
 * @par Example:
 * @code
 * #if H_PRESENT_IN_ESP_IDF_5_4_0
 * p_a_sta->threshold.rssi_5g_adjustment = p_c_sta->threshold->rssi_5g_adjustment;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_PRESENT_IN_ESP_IDF_5_4_0 (1)

/**
 * @def H_PRESENT_IN_ESP_IDF_5_5_0
 * @brief Whether fields added in ESP-IDF v5.5.0 are present.
 * @details One; v5.5.4 is a v5.5 patch release. It gates the SAE extension
 * field of the AP and station configurations and the three VHT beamformee
 * bits in the HE bitmask.
 * @note Read-only build configuration.
 * @warning See ::H_PRESENT_IN_ESP_IDF_5_4_0 -- this is about layout
 *          agreement, not about whether the feature is wanted.
 * @par Example:
 * @code
 * #if H_PRESENT_IN_ESP_IDF_5_5_0
 * p_c_ap->sae_ext = p_a_ap->sae_ext;
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_PRESENT_IN_ESP_IDF_5_5_0 (1)

/**
 * @def H_WIFI_NEW_RESERVED_FIELD_NAMES
 * @brief Whether the reserved bitfields use their post-5.4 names.
 * @details One. ESP-IDF v5.4 renamed the station configuration's spare bits
 * to ``reserved1`` and ``reserved2``; the pre-5.4 spellings were
 * ``reserved`` and ``he_reserved``. v5.5.4 uses the new names.
 * @note Read-only build configuration.
 * @warning Only consulted when ::H_DECODE_WIFI_RESERVED_FIELD is 1; the
 *          wrong choice is a compile error naming a struct member.
 * @par Example:
 * @code
 * #if H_WIFI_NEW_RESERVED_FIELD_NAMES
 * p_a_sta->reserved1 = WIFI_STA_CONFIG_1_GET_RESERVED_VAL(p_c_sta->bitmask);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_NEW_RESERVED_FIELD_NAMES (1)

/**
 * @def H_DECODE_WIFI_RESERVED_FIELD
 * @brief Whether the host preserves the spare configuration bits.
 * @details One. The reserved bits round-trip between the application's
 * structure and the wire bitmask; carrying them means a configuration read
 * back from the co-processor and written straight out again is unchanged,
 * instead of quietly losing bits this IDF version does not name yet.
 * @note Read-only build configuration.
 * @warning Needs ::H_WIFI_NEW_RESERVED_FIELD_NAMES to match the image, or
 *          the decode names a member that does not exist.
 * @par Example:
 * @code
 * #if H_DECODE_WIFI_RESERVED_FIELD
 * p_a_sta->reserved2 = WIFI_STA_CONFIG_2_GET_RESERVED_VAL(p_c_sta->he_bitmask);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_DECODE_WIFI_RESERVED_FIELD (1)

/* ----------------------------------------------------------------------- */
/* Device Provisioning Protocol (Wi-Fi Easy Connect) */
/* ----------------------------------------------------------------------- */

/**
 * @def H_DPP_SUPPORT
 * @brief Whether the host offers the DPP provisioning API.
 * @details Zero. ESP-IDF leaves ``CONFIG_WPA_DPP_SUPPORT`` off by default
 * and ``coprocessor/esp32c6/sdkconfig.defaults`` does not turn it on, so the
 * DPP bootstrap and listen calls are not in the image the host talks to.
 * @note Read-only build configuration.
 * @warning This also controls whether ``esp_dpp.h`` is included by the RPC
 *          headers; a 1 here needs the header and the co-processor support
 *          together.
 * @par Example:
 * @code
 * #if H_DPP_SUPPORT
 * #include "esp_dpp.h"
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_DPP_SUPPORT (0)

/**
 * @def H_SUPP_DPP_SUPPORT
 * @brief Whether the supplicant half of DPP is present.
 * @details Zero, matching ::H_DPP_SUPPORT. It is a separate upstream switch
 * because the supplicant callbacks and the application API were added at
 * different times; on this image neither exists.
 * @note Read-only build configuration.
 * @warning Enabling this alone gives the event path a DPP callback with no
 *          API to drive it.
 * @par Example:
 * @code
 * #if H_SUPP_DPP_SUPPORT
 * dispatch_dpp_event(evt);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_SUPP_DPP_SUPPORT (0)

/**
 * @def H_WIFI_DPP_SUPPORT
 * @brief Whether the Wi-Fi layer forwards DPP events.
 * @details Zero, matching ::H_DPP_SUPPORT. The third of the upstream DPP
 * switches; all three describe the same absent feature from three layers.
 * @note Read-only build configuration.
 * @warning All three DPP switches must agree; a mismatch compiles a
 *          dispatcher for events that are never encoded.
 * @par Example:
 * @code
 * #if H_WIFI_DPP_SUPPORT
 * forward_dpp_uri_ready(evt);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_WIFI_DPP_SUPPORT (0)

/**
 * @def H_DPP_URI_LEN_MAX
 * @brief Longest DPP bootstrapping URI the host would accept, in characters.
 * @details 255. The vendored header derives ``DPP_URI_LEN_MAX`` as this plus
 * one for the terminator, so the buffer is a round 256 bytes; a DPP URI
 * carries a curve name, a base64 public key and optional channel and MAC
 * hints, which fits comfortably.
 * @note Read-only build configuration.
 * @warning The buffer is a fixed array in a structure, so this is an
 *          allocation size, not a policy limit; shrinking it truncates URIs
 *          rather than rejecting them.
 * @par Example:
 * @code
 * #define DPP_URI_LEN_MAX (H_DPP_URI_LEN_MAX + 1)
 * @endcode
 * @since 0.1.0
 */
#define H_DPP_URI_LEN_MAX (255)

/* ----------------------------------------------------------------------- */
/* Co-processor-side peripherals the host could borrow */
/* ----------------------------------------------------------------------- */

/**
 * @def H_EXT_COEX_SUPPORT
 * @brief Whether the host can configure external radio coexistence.
 * @details Zero. External coexistence arbitrates the 2.4 GHz medium with
 * another radio over dedicated request/grant/priority wires; this harness
 * has no second radio and no coexistence pins between the RA8 and the C6.
 * @note Read-only build configuration.
 * @warning Controls inclusion of ``esp_hosted_cp_ext_coex.h`` as well as the
 *          RPC cases; the header and the flag move together.
 * @par Example:
 * @code
 * #if H_EXT_COEX_SUPPORT
 * #include "esp_hosted_cp_ext_coex.h"
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_EXT_COEX_SUPPORT (0)

/**
 * @def H_EXT_COEX_ADVANCE_SUPPORT
 * @brief Whether the extended coexistence parameter set is available.
 * @details Zero, matching ::H_EXT_COEX_SUPPORT; the advanced parameters are
 * a superset of a feature this board does not wire at all.
 * @note Read-only build configuration.
 * @warning Enabling this without ::H_EXT_COEX_SUPPORT compiles the advanced
 *          RPC case with no basic configuration behind it.
 * @par Example:
 * @code
 * #if H_EXT_COEX_ADVANCE_SUPPORT
 * encode_advanced_coex(req);
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_EXT_COEX_ADVANCE_SUPPORT (0)

/**
 * @def H_GPIO_EXPANDER_SUPPORT
 * @brief Whether the host drives the co-processor's spare GPIOs.
 * @details Zero. It would let the RA8 use unused C6 pins as a remote GPIO
 * expander over RPC. The C6 module here has no spare pins broken out -- the
 * seven the harness uses are the seven that are wired -- so the feature has
 * nothing to expand onto.
 * @note Read-only build configuration.
 * @warning Controls inclusion of ``esp_hosted_cp_gpio.h`` in the RPC wrapper
 *          headers as well as the RPC cases themselves.
 * @par Example:
 * @code
 * #if H_GPIO_EXPANDER_SUPPORT
 * #include "esp_hosted_cp_gpio.h"
 * #endif
 * @endcode
 * @since 0.1.0
 */
#define H_GPIO_EXPANDER_SUPPORT (0)
