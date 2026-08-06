/**
 * @file main.c
 * @brief ThreadX Module hello world example.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "txm_module.h"
#include "ra8_app_verify.h"
#include "ra8_err.h"
#include <stdio.h>

/* Define the Module Manager objects. */
static TXM_MODULE_MANAGER g_my_module_manager;
static TXM_MODULE g_my_module;

/* Dummy public key (32 bytes) */
static const uint8_t g_dummy_pubkey[32] = {0};

/* Example binary buffer (could be loaded from SD card in real use) */
extern const uint8_t hello_module_bin[];
extern const uint32_t hello_module_bin_len;

/* Memory area for the module manager */
#define MODULE_MANAGER_MEMORY_SIZE 65536
static uint8_t g_module_manager_memory[MODULE_MANAGER_MEMORY_SIZE];

/**
 * @brief Application entry point after ThreadX initialization.
 * @param first_unused_memory Pointer to first unused memory.
 */
void tx_application_define(void *first_unused_memory)
{
    (void)first_unused_memory;
    
    printf("Starting ThreadX Module Manager...\n");
    
    /* Initialize module manager */
    UINT status = txm_module_manager_initialize((VOID *)g_module_manager_memory, MODULE_MANAGER_MEMORY_SIZE);
    if (status != TX_SUCCESS) {
        printf("Failed to initialize module manager: %u\n", status);
        return;
    }
    
    printf("Verifying module signature...\n");
    /* Verify the module */
    ra8_err_t err = ra8_app_verify(hello_module_bin, hello_module_bin_len, g_dummy_pubkey);
    if (err == k_ra8_ok) {
        printf("Signature OK. Loading module...\n");
        /* Load the module */
        status = txm_module_manager_in_place_load(&g_my_module, "hello_module", (VOID *)hello_module_bin);
        if (status == TX_SUCCESS) {
            printf("Module loaded. Starting module...\n");
            /* Start the module */
            txm_module_manager_start(&g_my_module);
        } else {
            printf("Failed to load module: %u\n", status);
        }
    } else {
        printf("Signature verification failed: %d\n", err);
    }
}
