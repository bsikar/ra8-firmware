var emu__run_8h =
[
    [ "emu_run_cfg_t", "structemu__run__cfg__t.html", "structemu__run__cfg__t" ],
    [ "run_guards_t", "structrun__guards__t.html", "structrun__guards__t" ],
    [ "emu_budget_t", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247", [
      [ "k_run_chunk_insns", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a57842db613c489813b7dab328a8691a8", null ],
      [ "k_low_power_div", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a66885b4de24d05b1a213c0283504663b", null ],
      [ "k_run_max_chunks", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a5d3e040fd38be7232868b3c373a033bc", null ],
      [ "k_idle_spin_insns", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247aa9685a227e91de5263795b7b9bb8ff4e", null ],
      [ "k_op_branch_self", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247aaf379b71e187b67c4ce9712c8f2b7da2", null ],
      [ "k_op_wfi", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a3e2704013fdfb6433bec959ff415658d", null ],
      [ "k_op_cpsie_i", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247ad1d97124b01477d87f8dafddfc1ab70a", null ],
      [ "k_op_bn_mask", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247aa6c4c516874a2660e0c99e0d7571333c", null ],
      [ "k_op_bn_base", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247ace4e4191f9810b372d894d38464748bf", null ],
      [ "k_op_bn_imm", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247ade88f5bc5f13d71aeab719a29af10bf5", null ],
      [ "k_bn_imm_sext_shl", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a7d15a017e8759925b7fe692c4d47faab", null ],
      [ "k_bn_imm_sext_shr", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a8359ffd4a5e6304a137ed8840ac4e5cc", null ],
      [ "k_idle_scan_fwd", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a677b65903e9552c47790d15a495e65a9", null ],
      [ "k_idle_loop_max", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a4095f6debcebb2d5d7f80b9a534c3402", null ],
      [ "k_run_wall_s", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a42b07dee4493397b937079762a6df9a2", null ],
      [ "k_run_inner_max", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a14b1846d9915b0c1c35a40df285cda38", null ],
      [ "k_env_strtol_base", "emu__run_8h.html#a674fd6080a95173fa8ba9ed9468a3247a26808afafc0a5f0a1049d05c239ea1f4", null ]
    ] ],
    [ "emu_exit_t", "emu__run_8h.html#a525c35bc9f4fb62e1438d19fb29e6b5c", [
      [ "k_emu_exit_ok", "emu__run_8h.html#a525c35bc9f4fb62e1438d19fb29e6b5ca1d1a3b5cc89ba2f565dd59dade9d0d2c", null ],
      [ "k_emu_exit_fault", "emu__run_8h.html#a525c35bc9f4fb62e1438d19fb29e6b5cac6d701a782880dda0a3a9596ac1ed99f", null ],
      [ "k_emu_exit_bkpt", "emu__run_8h.html#a525c35bc9f4fb62e1438d19fb29e6b5ca5b9d548947068b09dc2d0a0afa6160e7", null ],
      [ "k_emu_exit_timeout", "emu__run_8h.html#a525c35bc9f4fb62e1438d19fb29e6b5cab00c316343cbcfd23b48727c1b94bd83", null ]
    ] ],
    [ "emu_misc_t", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92", [
      [ "k_thumb_op5_shift", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92adc6ba74138472acca2ad742e3d8d3918", null ],
      [ "k_thumb_op5_mask", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92adc13ddeab261c0907c73a07176e1e387", null ],
      [ "k_thumb32_op5_min", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92aa9e01a584a751cccbfc4bd509dd3da67", null ],
      [ "k_cs_op_shift", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92a051b310d522256b9413faf43598d7cfe", null ],
      [ "k_cs_op_mask", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92ae7516b16ce395a9275c9f28211e7b611", null ],
      [ "k_max_panel_px", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92a5050857306ab5dbaf7fc4f4f7c65af1a", null ],
      [ "k_record_dir_mode", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92a182bea4e4fc862d02039ec92747e9c22", null ],
      [ "k_dump_sym_max", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92af292999a64c6e906aec985465a9eddbd", null ],
      [ "k_sectors_per_mib", "emu__run_8h.html#a7d8f03374fc5ae8de5bd554a66810d92a326d2593dc876ae180956713e3d5ddbd", null ]
    ] ],
    [ "emu_size_t", "emu__run_8h.html#af05123e6cca6a7c0a8aba2f91afaca7f", [
      [ "k_bytes_per_sector", "emu__run_8h.html#af05123e6cca6a7c0a8aba2f91afaca7fad9dbfa577a6bd4b21c7c8bf4d36d8f12", null ],
      [ "k_size_kib", "emu__run_8h.html#af05123e6cca6a7c0a8aba2f91afaca7faeee38805cb43462a556a6272fa38307a", null ],
      [ "k_sd_u32_max", "emu__run_8h.html#af05123e6cca6a7c0a8aba2f91afaca7faf07e44b8913cefd92105ecb91ba19eb8", null ],
      [ "k_fat32_min_mib", "emu__run_8h.html#af05123e6cca6a7c0a8aba2f91afaca7fa652547c7d81736f5f6248cdb45041f01", null ]
    ] ],
    [ "emu_run_and_report", "emu__run_8h.html#a50638908e5794ffedea93d9ab9e49609", null ],
    [ "run_read_guards", "emu__run_8h.html#ad4521e96cea3e2b9c58e45c4ed2cdcdb", null ]
];