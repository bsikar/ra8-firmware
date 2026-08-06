/**
 * @file ra8_mdl_iface.h
 * @brief Interfaces for media downloader network and storage layers.
 */
#pragma once

#include <stdint.h>
#include "ra8_err.h"

/** @brief Network interface for media download */
typedef struct {
    ra8_err_t (*http_get)(void* ctx, const char* url, uint8_t* buf, uint32_t cap, uint32_t* got);
    ra8_err_t (*http_head)(void* ctx, const char* url, uint32_t* content_length);
    void* ctx;
} ra8_mdl_net_iface_t;

/** @brief Storage interface for media download */
typedef struct {
    ra8_err_t (*write)(void* ctx, const char* path, const uint8_t* data, uint32_t len);
    ra8_err_t (*mkdir)(void* ctx, const char* path);
    void* ctx;
} ra8_mdl_storage_iface_t;
