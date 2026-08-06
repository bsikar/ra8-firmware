/**
 * @file ra8_c6link_mdl.c
 * @brief Client-side implementation for media download via ESP32-C6 RPC.
 */

#include "ra8_c6link_mdl.h"

ra8_err_t ra8_c6link_mdl_download(ra8_c6link_t* link, const char* url, const char* output_path) {
    if (!link || !url || !output_path) {
        return k_ra8_err_invalid_arg;
    }
    // TODO: implement RPC request to ESP32-C6
    return k_ra8_ok;
}

ra8_err_t ra8_c6link_mdl_poll(ra8_c6link_t* link, MdlDownloadProgress* progress) {
    if (!link || !progress) {
        return k_ra8_err_invalid_arg;
    }
    // TODO: implement RPC poll from ESP32-C6
    return k_ra8_ok;
}

ra8_err_t ra8_c6link_mdl_cancel(ra8_c6link_t* link) {
    if (!link) {
        return k_ra8_err_invalid_arg;
    }
    // TODO: implement RPC cancel to ESP32-C6
    return k_ra8_ok;
}
