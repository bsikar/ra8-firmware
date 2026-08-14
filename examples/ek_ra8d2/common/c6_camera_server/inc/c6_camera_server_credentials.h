/**
 * @file c6_camera_server_credentials.h
 * @brief Wi-Fi credential declarations backed by the private build tree.
 * @details The build generates definitions from OpenBao-backed configuration;
 *          source files consume only these declarations and contain no secret.
 *
 * @details Declares the station credentials generated from the private CMake
 *          inputs. The public source tree contains no credential values.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/** @brief Configured station SSID, generated into the private build tree.
 *  @since 0.1.0 */
extern const char g_c6_cam_wifi_ssid[];

/** @brief Configured station PSK, generated into the private build tree.
 *  @since 0.1.0 */
extern const char g_c6_cam_wifi_psk[];
