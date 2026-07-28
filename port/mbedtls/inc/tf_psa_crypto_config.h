/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

/**
 * \file tf_psa_crypto_config.h
 * \brief PSA crypto configuration options (set of defines)
 *
 */
/**
 * This file determines which cryptographic mechanisms are enabled
 * through the PSA Cryptography API (\c psa_xxx() functions).
 *
 * To enable a cryptographic mechanism, uncomment the definition of
 * the corresponding \c PSA_WANT_xxx preprocessor symbol.
 * To disable a cryptographic mechanism, comment out the definition of
 * the corresponding \c PSA_WANT_xxx preprocessor symbol.
 * The names of cryptographic mechanisms correspond to values
 * defined in psa/crypto_values.h, with the prefix \c PSA_WANT_ instead
 * of \c PSA_.
 *
 * Note that many cryptographic mechanisms involve two symbols: one for
 * the key type (\c PSA_WANT_KEY_TYPE_xxx) and one for the algorithm
 * (\c PSA_WANT_ALG_xxx). Mechanisms with additional parameters may involve
 * additional symbols.
 */

/*
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

/*
 * C23HDR-OK: this is NOT an ordinary include guard and must not become
 * `#pragma once`. `PSA_CRYPTO_CONFIG_H` is a vendor-ABI sentinel: the
 * vendored Mbed TLS SOUP (mbedtls_config_check_before.h /
 * mbedtls_config_check_user.h) tests `#if defined(PSA_CRYPTO_CONFIG_H)` to
 * confirm the PSA crypto config file was supplied, so the macro must keep
 * being defined here.
 */
#ifndef PSA_CRYPTO_CONFIG_H
/** @brief PSA CRYPTO CONFIG h. */
#define PSA_CRYPTO_CONFIG_H

/*
 * Thin umbrella header.
 *
 * The configuration body was split by concern into the sub-headers
 * included below so that no single header exceeds the repository's
 * 1000-line limit. The split is a pure move of preprocessor `#define`s
 * (and their documentation) -- the umbrella keeps the original file
 * name and include guard, so every consumer that
 * `#include "tf_psa_crypto_config.h"` is unchanged.
 *
 * - tf_psa_crypto_config_mechanisms.h : cryptographic-mechanism
 *   selection (PSA API + extended API), data-format support, PSA core.
 * - tf_psa_crypto_config_platform.h   : platform abstraction layer and
 *   general / test configuration options.
 * - tf_psa_crypto_config_drivers.h    : builtin-driver tuning options
 *   and the trailing removed-mechanisms testing switch.
 */
#include "tf_psa_crypto_config_drivers.h"
#include "tf_psa_crypto_config_mechanisms.h"
#include "tf_psa_crypto_config_platform.h"

#endif /* PSA_CRYPTO_CONFIG_H */
