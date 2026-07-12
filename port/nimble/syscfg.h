/**
 * @file syscfg.h
 * @brief NimBLE syscfg overrides for the RA8D2 port.
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
#define MYNEWT_VAL_BLE_SM (1)

#undef MYNEWT_VAL_BLE_SM_LEGACY
#define MYNEWT_VAL_BLE_SM_LEGACY (1)

#undef MYNEWT_VAL_BLE_SM_SC
#define MYNEWT_VAL_BLE_SM_SC (1)

#undef MYNEWT_VAL_BLE_SM_SC_ONLY
#define MYNEWT_VAL_BLE_SM_SC_ONLY (0)

#undef MYNEWT_VAL_BLE_SM_BONDING
#define MYNEWT_VAL_BLE_SM_BONDING (1)

#undef MYNEWT_VAL_BLE_SM_MITM
#define MYNEWT_VAL_BLE_SM_MITM (1)

#undef MYNEWT_VAL_BLE_SM_IO_CAP
#define MYNEWT_VAL_BLE_SM_IO_CAP (BLE_HS_IO_NO_INPUT_OUTPUT)

#undef MYNEWT_VAL_BLE_SM_KEYPRESS
#define MYNEWT_VAL_BLE_SM_KEYPRESS (0)

#undef MYNEWT_VAL_BLE_SM_OOB_DATA_FLAG
#define MYNEWT_VAL_BLE_SM_OOB_DATA_FLAG (0)

#undef MYNEWT_VAL_BLE_SM_MAX_PROCS
#define MYNEWT_VAL_BLE_SM_MAX_PROCS (1)

#undef MYNEWT_VAL_BLE_STORE_MAX_BONDS
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS (4)

#undef MYNEWT_VAL_BLE_STORE_MAX_CCCDS
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS (8)

/* ----------------------------------------------------------------------- */
/* GATT client */
/* ----------------------------------------------------------------------- */

#undef MYNEWT_VAL_BLE_GATT_DISC_ALL_SVCS
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_SVCS (1)

#undef MYNEWT_VAL_BLE_GATT_DISC_SVC_UUID
#define MYNEWT_VAL_BLE_GATT_DISC_SVC_UUID (1)

#undef MYNEWT_VAL_BLE_GATT_DISC_ALL_CHRS
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_CHRS (1)

#undef MYNEWT_VAL_BLE_GATT_DISC_ALL_DSCS
#define MYNEWT_VAL_BLE_GATT_DISC_ALL_DSCS (1)

#undef MYNEWT_VAL_BLE_GATT_READ
#define MYNEWT_VAL_BLE_GATT_READ (1)

#undef MYNEWT_VAL_BLE_GATT_WRITE
#define MYNEWT_VAL_BLE_GATT_WRITE (1)

#undef MYNEWT_VAL_BLE_GATT_WRITE_NO_RSP
#define MYNEWT_VAL_BLE_GATT_WRITE_NO_RSP (1)

#undef MYNEWT_VAL_BLE_GATT_NOTIFY
#define MYNEWT_VAL_BLE_GATT_NOTIFY (1)

#undef MYNEWT_VAL_BLE_ROLE_CENTRAL
#define MYNEWT_VAL_BLE_ROLE_CENTRAL (1)

#undef MYNEWT_VAL_BLE_ROLE_OBSERVER
#define MYNEWT_VAL_BLE_ROLE_OBSERVER (1)

/* ----------------------------------------------------------------------- */
/* Mesh profile */
/* ----------------------------------------------------------------------- */

#undef MYNEWT_VAL_BLE_MESH
#define MYNEWT_VAL_BLE_MESH (1)

#undef MYNEWT_VAL_BLE_MESH_PROV
#define MYNEWT_VAL_BLE_MESH_PROV (1)

#undef MYNEWT_VAL_BLE_MESH_PB_ADV
#define MYNEWT_VAL_BLE_MESH_PB_ADV (1)

#undef MYNEWT_VAL_BLE_MESH_PB_GATT
#define MYNEWT_VAL_BLE_MESH_PB_GATT (1)

#undef MYNEWT_VAL_BLE_MESH_PROXY
#define MYNEWT_VAL_BLE_MESH_PROXY (1)

#undef MYNEWT_VAL_BLE_MESH_RELAY
#define MYNEWT_VAL_BLE_MESH_RELAY (1)

#undef MYNEWT_VAL_BLE_MESH_FRIEND
#define MYNEWT_VAL_BLE_MESH_FRIEND (0)

#undef MYNEWT_VAL_BLE_MESH_LOW_POWER
#define MYNEWT_VAL_BLE_MESH_LOW_POWER (0)

#undef MYNEWT_VAL_BLE_MESH_CRPL
#define MYNEWT_VAL_BLE_MESH_CRPL (10)

#undef MYNEWT_VAL_BLE_MESH_APP_KEY_COUNT
#define MYNEWT_VAL_BLE_MESH_APP_KEY_COUNT (1)

#undef MYNEWT_VAL_BLE_MESH_SUBNET_COUNT
#define MYNEWT_VAL_BLE_MESH_SUBNET_COUNT (1)

/* ----------------------------------------------------------------------- */
/* Hardware crypto offload via ra8_rsip */
/* ----------------------------------------------------------------------- */

/**
 * When set, ble_sm_alg.c calls into our ra8_rsip wrapper for the P-256
 * key-pair generation, ECDH shared-secret, AES-CMAC and AES-128-ECB
 * primitives instead of the bundled tinycrypt fallback. The shim
 * lives in libs/ra8_ble_host/src/ra8_ble_security.c and is wired via
 * the public ble_sm_alg_*_hw hooks defined upstream when this is set.
 */
#undef MYNEWT_VAL_BLE_SM_SC_HW_OFFLOAD
#define MYNEWT_VAL_BLE_SM_SC_HW_OFFLOAD (1)
