var ra8__ota_8h =
[
    [ "ra8_ota_manifest_t", "structra8__ota__manifest__t.html", "structra8__ota__manifest__t" ],
    [ "ra8_ota_progress_t", "structra8__ota__progress__t.html", "structra8__ota__progress__t" ],
    [ "ra8_ota_net_iface_t", "structra8__ota__net__iface__t.html", "structra8__ota__net__iface__t" ],
    [ "ra8_ota_crypto_iface_t", "structra8__ota__crypto__iface__t.html", "structra8__ota__crypto__iface__t" ],
    [ "ra8_ota_flash_iface_t", "structra8__ota__flash__iface__t.html", "structra8__ota__flash__iface__t" ],
    [ "ra8_ota_cfg_t", "structra8__ota__cfg__t.html", "structra8__ota__cfg__t" ],
    [ "ra8_ota_progress_cb_t", "ra8__ota_8h.html#a6b7bc0cce05df11b689432cd65335cc4", null ],
    [ "ra8_ota_constants_t", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33a", [
      [ "k_ra8_ota_chunk_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aa9339ac5cb84d047dc9d65ebccb0b0c5e", null ],
      [ "k_ra8_ota_manifest_max_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aaea5a192e6d7d7d579e1b2450736b8eec", null ],
      [ "k_ra8_ota_sha256_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aa3b9036cdf5348bca912c013c565a5891", null ],
      [ "k_ra8_ota_signature_max_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aab605d8c9cf35a6a7f1ce1200fe21c786", null ],
      [ "k_ra8_ota_url_max_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aa375eeb2f8ccf36d159f09cc024fa813d", null ],
      [ "k_ra8_ota_version_str_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aa75d901475509dffb8b43f518657cd9ab", null ],
      [ "k_ra8_ota_max_image_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aaa3e24813c56c2f6c5b274b5c04f569f7", null ],
      [ "k_ra8_ota_thread_stack_bytes", "ra8__ota_8h.html#acc163f4696e376371178a507d1e1b33aa91e969414f64ed58bfe4cd024d160369", null ]
    ] ],
    [ "ra8_ota_state_t", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210", [
      [ "k_ra8_ota_state_idle", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210add1accf00d40d32573639d5e32c1a581", null ],
      [ "k_ra8_ota_state_checking", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210a3c4a1d1660e51e22b615416362a7b175", null ],
      [ "k_ra8_ota_state_downloading", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210ac9e18a29632599d80985e733b0d811c9", null ],
      [ "k_ra8_ota_state_verifying", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210a4b599f93906ab13cb4e6a84a0dcbf57a", null ],
      [ "k_ra8_ota_state_committing", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210aa9eff86fbd8e767aed8d319bb4311cdd", null ],
      [ "k_ra8_ota_state_done", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210a4ef467297c72115cfd7cd169606f86cd", null ],
      [ "k_ra8_ota_state_error", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210a32e09b27ccfbe7443a162577f9bd697e", null ],
      [ "k_ra8_ota_state_count", "ra8__ota_8h.html#a942c67a14d02efefeebbf76b94d35210a3096b6674ed85d6f8ac222d34a01f26c", null ]
    ] ],
    [ "ra8_ota_check_for_update", "ra8__ota_8h.html#ac78a1d94ba4060a948c1e811120a930f", null ],
    [ "ra8_ota_commit_and_reboot", "ra8__ota_8h.html#a13f54e3197233147b1e0c307edb3364e", null ],
    [ "ra8_ota_deinit", "ra8__ota_8h.html#afb31e22a000c115482771e0429aa823c", null ],
    [ "ra8_ota_download_to_inactive_bank", "ra8__ota_8h.html#a720345a656395ab8b18f7c7fadb2ac57", null ],
    [ "ra8_ota_get_state", "ra8__ota_8h.html#aa79a91f79639a1d2f73d5e6182332d46", null ],
    [ "ra8_ota_init", "ra8__ota_8h.html#aa49ead323727aba4ad1fcafb87138d2e", null ],
    [ "ra8_ota_run_full_update", "ra8__ota_8h.html#aa05dcce9cb28aac1be70bb0968249d93", null ],
    [ "ra8_ota_run_step", "ra8__ota_8h.html#a0b95dad2d7d8dc754500470bb765a85c", null ],
    [ "ra8_ota_system_reset_hook", "ra8__ota_8h.html#a166ee92aa2ad933e071a6ac03f710a79", null ],
    [ "ra8_ota_verify_signature", "ra8__ota_8h.html#ab0f4f73d2c29c1e5f9d0a4e5d1a04bdb", null ]
];