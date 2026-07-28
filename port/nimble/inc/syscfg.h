/**
 * @file port/nimble/inc/syscfg.h
 * @brief NimBLE syscfg overrides for the RA8 port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Apache NimBLE consumes a ``syscfg.h`` from the build's include search
 * path; if absent it falls back to ``porting/nimble/include/syscfg/syscfg.h``
 * which leaves SMP, bonding and Mesh disabled. We override the relevant
 * settings for this port:
 *
 *   - ``MYNEWT_VAL_BLE_SM`` / ``MYNEWT_VAL_BLE_SM_LEGACY`` /
 *     ``MYNEWT_VAL_BLE_SM_SC`` -- enable the Security Manager
 *     Protocol with both Legacy Pairing and LE Secure Connections.
 *   - ``MYNEWT_VAL_BLE_SM_BONDING`` -- enable bonding so keys persist
 *     across power cycles via the host key store.
 *   - ``MYNEWT_VAL_BLE_SM_MITM`` -- allow Man-in-the-Middle protected
 *     pairing methods (Passkey Entry / Numeric Comparison).
 *   - ``MYNEWT_VAL_BLE_SM_SC_ONLY`` -- 0; fall back to legacy when the
 *     remote does not support LE Secure Connections.
 *   - ``MYNEWT_VAL_BLE_MESH`` -- enable the upstream NimBLE Mesh
 *     stack so apps can register elements and models.
 *   - ``MYNEWT_VAL_BLE_GATT_CLIENT`` -- expose the GATT client peer
 *     APIs so central-role apps can discover and read/write services.
 *
 * The values are forwarded to NimBLE through the
 * ``MYNEWT_VAL(...)`` macro which expands to ``MYNEWT_VAL_##NAME``;
 * defining those identifiers here pre-empts the upstream defaults.
 *
 * @warning UNVALIDATED SCAFFOLD (issue #286): these syscfg knobs enable
 * NimBLE features (SMP, bonding, Mesh, GATT client) that link and pass the
 * static gates, but the whole NimBLE port has NEVER been hardware-validated
 * and is NOT emulator-gated -- ra8_emulator models no RA8D2 BLE controller / HCI
 * mailbox, and the underlying ra8_ble transport is itself unproven on this
 * board (see #86, #91). Enabling a knob here does not imply the feature
 * runs on silicon; consumers stay under ``examples/_unsupported/`` until a
 * NimBLE app is driven to real hardware validation and promoted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Pull in the upstream defaults so settings we do not override remain
 * sensible. Order matters: the upstream header uses ``#ifndef`` guards,
 * so our overrides must come AFTER it but the include itself is here
 * to avoid unguarded symbols leaking. NimBLE's build wrapper expects
 * a single syscfg.h on the include path; including the upstream copy
 * here keeps every other knob at its documented default. */
#include "../../libs/third_party/nimble/porting/nimble/include/syscfg/syscfg.h"

/* ----------------------------------------------------------------------- */
/* Security Manager Protocol (SMP) */
/* ----------------------------------------------------------------------- */

#undef MYNEWT_VAL_BLE_SM
/** @brief MYNEWT VAL BLE SM. */
#define MYNEWT_VAL_BLE_SM (1)

#undef MYNEWT_VAL_BLE_SM_LEGACY
/** @brief MYNEWT VAL BLE SM LEGACY. */
#define MYNEWT_VAL_BLE_SM_LEGACY (1)

#undef MYNEWT_VAL_BLE_SM_SC
/** @brief MYNEWT VAL BLE SM SC. */
#define MYNEWT_VAL_BLE_SM_SC (1)

#undef MYNEWT_VAL_BLE_SM_SC_ONLY
/** @brief MYNEWT VAL BLE SM SC ONLY. */
#define MYNEWT_VAL_BLE_SM_SC_ONLY (0)

#undef MYNEWT_VAL_BLE_SM_BONDING
/** @brief MYNEWT VAL BLE SM BONDING. */
#define MYNEWT_VAL_BLE_SM_BONDING (1)

#undef MYNEWT_VAL_BLE_SM_MITM
/** @brief MYNEWT VAL BLE SM MITM. */
#define MYNEWT_VAL_BLE_SM_MITM (1)

#undef MYNEWT_VAL_BLE_SM_IO_CAP
/** @brief MYNEWT VAL BLE SM IO CAP. */
#define MYNEWT_VAL_BLE_SM_IO_CAP (BLE_HS_IO_NO_INPUT_OUTPUT)

#undef MYNEWT_VAL_BLE_SM_KEYPRESS
/** @brief MYNEWT VAL BLE SM KEYPRESS. */
#define MYNEWT_VAL_BLE_SM_KEYPRESS (0)

#undef MYNEWT_VAL_BLE_SM_OOB_DATA_FLAG
/** @brief MYNEWT VAL BLE SM OOB DATA FLAG. */
#define MYNEWT_VAL_BLE_SM_OOB_DATA_FLAG (0)

#undef MYNEWT_VAL_BLE_SM_MAX_PROCS
/** @brief MYNEWT VAL BLE SM MAX PROCS. */
#define MYNEWT_VAL_BLE_SM_MAX_PROCS (1)

#undef MYNEWT_VAL_BLE_STORE_MAX_BONDS
/** @brief MYNEWT VAL BLE STORE MAX BONDS. */
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS (4)

#undef MYNEWT_VAL_BLE_STORE_MAX_CCCDS
/** @brief MYNEWT VAL BLE STORE MAX CCCDS. */
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS (8)

/* ----------------------------------------------------------------------- */
/* GATT client */
/* ----------------------------------------------------------------------- */

#undef MYNEWT_VAL_BLE_GATT_DISC_ALL_SVCS
/** @brief MYNEWT VAL BLE GATT DISC ALL SVCS. */
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_SVCS (1)

#undef MYNEWT_VAL_BLE_GATT_DISC_SVC_UUID
/** @brief MYNEWT VAL BLE GATT DISC SVC UUID. */
#define MYNEWT_VAL_BLE_GATT_DISC_SVC_UUID (1)

#undef MYNEWT_VAL_BLE_GATT_DISC_ALL_CHRS
/** @brief MYNEWT VAL BLE GATT DISC ALL CHRS. */
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_CHRS (1)

#undef MYNEWT_VAL_BLE_GATT_DISC_ALL_DSCS
/** @brief MYNEWT VAL BLE GATT DISC ALL DSCS. */
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_DSCS (1)

#undef MYNEWT_VAL_BLE_GATT_READ
/** @brief MYNEWT VAL BLE GATT READ. */
#define MYNEWT_VAL_BLE_GATT_READ (1)

#undef MYNEWT_VAL_BLE_GATT_WRITE
/** @brief MYNEWT VAL BLE GATT WRITE. */
#define MYNEWT_VAL_BLE_GATT_WRITE (1)

#undef MYNEWT_VAL_BLE_GATT_WRITE_NO_RSP
/** @brief MYNEWT VAL BLE GATT WRITE NO RSP. */
#define MYNEWT_VAL_BLE_GATT_WRITE_NO_RSP (1)

#undef MYNEWT_VAL_BLE_GATT_NOTIFY
/** @brief MYNEWT VAL BLE GATT NOTIFY. */
#define MYNEWT_VAL_BLE_GATT_NOTIFY (1)

#undef MYNEWT_VAL_BLE_ROLE_CENTRAL
/** @brief MYNEWT VAL BLE ROLE CENTRAL. */
#define MYNEWT_VAL_BLE_ROLE_CENTRAL (1)

#undef MYNEWT_VAL_BLE_ROLE_OBSERVER
/** @brief MYNEWT VAL BLE ROLE OBSERVER. */
#define MYNEWT_VAL_BLE_ROLE_OBSERVER (1)

/* ----------------------------------------------------------------------- */
/* Mesh profile */
/* ----------------------------------------------------------------------- */

#undef MYNEWT_VAL_BLE_MESH
/** @brief MYNEWT VAL BLE MESH. */
#define MYNEWT_VAL_BLE_MESH (1)

#undef MYNEWT_VAL_BLE_MESH_PROV
/** @brief MYNEWT VAL BLE MESH PROV. */
#define MYNEWT_VAL_BLE_MESH_PROV (1)

#undef MYNEWT_VAL_BLE_MESH_PB_ADV
/** @brief MYNEWT VAL BLE MESH PB ADV. */
#define MYNEWT_VAL_BLE_MESH_PB_ADV (1)

#undef MYNEWT_VAL_BLE_MESH_PB_GATT
/** @brief MYNEWT VAL BLE MESH PB GATT. */
#define MYNEWT_VAL_BLE_MESH_PB_GATT (1)

#undef MYNEWT_VAL_BLE_MESH_PROXY
/** @brief MYNEWT VAL BLE MESH PROXY. */
#define MYNEWT_VAL_BLE_MESH_PROXY (1)

#undef MYNEWT_VAL_BLE_MESH_RELAY
/** @brief MYNEWT VAL BLE MESH RELAY. */
#define MYNEWT_VAL_BLE_MESH_RELAY (1)

#undef MYNEWT_VAL_BLE_MESH_FRIEND
/** @brief MYNEWT VAL BLE MESH FRIEND. */
#define MYNEWT_VAL_BLE_MESH_FRIEND (0)

#undef MYNEWT_VAL_BLE_MESH_LOW_POWER
/** @brief MYNEWT VAL BLE MESH LOW POWER. */
#define MYNEWT_VAL_BLE_MESH_LOW_POWER (0)

#undef MYNEWT_VAL_BLE_MESH_CRPL
/** @brief MYNEWT VAL BLE MESH CRPL. */
#define MYNEWT_VAL_BLE_MESH_CRPL (10)

#undef MYNEWT_VAL_BLE_MESH_APP_KEY_COUNT
/** @brief MYNEWT VAL BLE MESH APP KEY COUNT. */
#define MYNEWT_VAL_BLE_MESH_APP_KEY_COUNT (1)

#undef MYNEWT_VAL_BLE_MESH_SUBNET_COUNT
/** @brief MYNEWT VAL BLE MESH SUBNET COUNT. */
#define MYNEWT_VAL_BLE_MESH_SUBNET_COUNT (1)

/* ----------------------------------------------------------------------- */
/* Hardware crypto offload via ra8_rsip */
/* ----------------------------------------------------------------------- */

/**
 * Project syscfg reserved for routing NimBLE's P-256 key-pair
 * generation, ECDH shared-secret, AES-CMAC and AES-128-ECB primitives
 * through our ra8_rsip wrapper instead of the bundled tinycrypt
 * fallback. The offload shim previously lived in the first-party BLE-host
 * facade, which has been removed; upstream NimBLE does not consume this
 * value, so it is currently inert and NimBLE uses its tinycrypt
 * fallback. The offload path will be re-wired when the ESP32-C6
 * controller link and its crypto seam land.
 */
#undef MYNEWT_VAL_BLE_SM_SC_HW_OFFLOAD
/** @brief MYNEWT VAL BLE SM SC HW OFFLOAD. */
#define MYNEWT_VAL_BLE_SM_SC_HW_OFFLOAD (1)
