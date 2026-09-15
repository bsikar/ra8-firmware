var ux__dcd__ra8__usb__setup_8c =
[
    [ "UX_SOURCE_CODE", "ux__dcd__ra8__usb__setup_8c.html#ad65ae5121da702593d1c2ff0ea569eaf", null ],
    [ "ra8_dcd_dwt_t", "ux__dcd__ra8__usb__setup_8c.html#a1b95752a91404089f9ffa23fe099d840", [
      [ "k_dcd_dwt_cyccnt_addr", "ux__dcd__ra8__usb__setup_8c.html#a1b95752a91404089f9ffa23fe099d840a54da9d1e7da4d134a4d027c8bb0b80f4", null ]
    ] ],
    [ "ra8_usb_dcp_brdy_t", "ux__dcd__ra8__usb__setup_8c.html#aa39358b915ee23e6be48a5d668afb3c7", [
      [ "k_ra8_usb_dcp_brdy_bit", "ux__dcd__ra8__usb__setup_8c.html#aa39358b915ee23e6be48a5d668afb3c7a0ac2e9ba7f4a831ec07e138f81726d49", null ]
    ] ],
    [ "ra8_usb_setup_fp_shift_t", "ux__dcd__ra8__usb__setup_8c.html#aa4f45853fe661976f2d3f3477ba49a7d", [
      [ "k_ra8_usb_fp_shift_usbleng", "ux__dcd__ra8__usb__setup_8c.html#aa4f45853fe661976f2d3f3477ba49a7da9215f62522ed5df0657fd24a7fa808c8", null ]
    ] ],
    [ "ra8_usb_setup_local_t", "ux__dcd__ra8__usb__setup_8c.html#a72fb3424f5c7c6147c85cdfce5d5ade4", [
      [ "k_ra8_usb_setup_dir_mask", "ux__dcd__ra8__usb__setup_8c.html#a72fb3424f5c7c6147c85cdfce5d5ade4aa6ddf6f0068b01149615d7e7b6e6e201", null ],
      [ "k_ra8_usb_breq_set_address", "ux__dcd__ra8__usb__setup_8c.html#a72fb3424f5c7c6147c85cdfce5d5ade4ab98f6180f6fb9e505ca76f79202825d2", null ]
    ] ],
    [ "ra8_usb_setup_usbreq_t", "ux__dcd__ra8__usb__setup_8c.html#aed1e063c6abab142ee245139a9bc985c", [
      [ "k_ra8_usb_usbreq_breq_mask", "ux__dcd__ra8__usb__setup_8c.html#aed1e063c6abab142ee245139a9bc985cab817d9b0f97626467aefd07595b856e4", null ],
      [ "k_ra8_usb_usbreq_bmrt_mask", "ux__dcd__ra8__usb__setup_8c.html#aed1e063c6abab142ee245139a9bc985ca97935cda09614a2da2aa1e2b9baca0b5", null ],
      [ "k_ra8_usb_usbreq_set_addr", "ux__dcd__ra8__usb__setup_8c.html#aed1e063c6abab142ee245139a9bc985cae4afa851fb5947ee06a35215df5f577b", null ]
    ] ],
    [ "internal_ctrt_dispatch_fresh_setup", "ux__dcd__ra8__usb__setup_8c.html#ae216ac52770b2f6437caa39687566f5c", null ],
    [ "internal_ctrt_handle_valid", "ux__dcd__ra8__usb__setup_8c.html#ae50846cef0a66e4a24692e09410721d3", null ],
    [ "internal_pack_setup_le", "ux__dcd__ra8__usb__setup_8c.html#a12da6884f01e2acc64ec5b293e59a579", null ],
    [ "internal_try_defer_ctrl_out", "ux__dcd__ra8__usb__setup_8c.html#a966f8d03cf40bbabb8407ab8019ef018", null ],
    [ "priv_dispatch_setup", "ux__dcd__ra8__usb__setup_8c.html#af8d703a29b71c3901f6679a59f737215", null ],
    [ "priv_handle_ctrl_out_data", "ux__dcd__ra8__usb__setup_8c.html#af9653222cf4fa3c21600727dba62eb44", null ],
    [ "priv_handle_ctrt", "ux__dcd__ra8__usb__setup_8c.html#a9b92f0f6a46a4d0805a180d3e54a214b", null ],
    [ "priv_trace_event", "ux__dcd__ra8__usb__setup_8c.html#a2dc239992e461a12ba6e104ce3aacffa", null ],
    [ "g_ctrl_out_done", "ux__dcd__ra8__usb__setup_8c.html#a90a44955382c9e9eec5263d96eedef5a", null ],
    [ "g_ctrl_out_pending", "ux__dcd__ra8__usb__setup_8c.html#ae848629004cce45dfce3250881660386", null ],
    [ "g_ctrl_out_rx", "ux__dcd__ra8__usb__setup_8c.html#ac696e3f6801831ef811c5bd93dae758b", null ],
    [ "g_ctrl_out_tr", "ux__dcd__ra8__usb__setup_8c.html#ad4898dc80738375ae3efdfd497f6f433", null ],
    [ "g_ctrl_out_wlen", "ux__dcd__ra8__usb__setup_8c.html#a422184af4732010d06315bf1990fad7d", null ],
    [ "g_dispatch_skip_reason", "ux__dcd__ra8__usb__setup_8c.html#a761253efa714d8d3e3bf1c9615704d69", null ],
    [ "g_dispatched_fp_ring", "ux__dcd__ra8__usb__setup_8c.html#a3415ab38ff3eee6569e7906ee43fec94", null ],
    [ "g_dispatched_fp_ring_idx", "ux__dcd__ra8__usb__setup_8c.html#abfbd9ca887282cc8f03d228d282378e5", null ],
    [ "g_last_dispatched_setup_fp", "ux__dcd__ra8__usb__setup_8c.html#a0f096bd09b704f3af9a0977bbd91355b", null ],
    [ "g_setup_dispatch_count", "ux__dcd__ra8__usb__setup_8c.html#aa06dbe50f219d12383e7dffe128c5117", null ],
    [ "g_setup_packet_buffer", "ux__dcd__ra8__usb__setup_8c.html#a2d2f598e8095f9a808c9f159e8af3794", null ],
    [ "g_setup_packet_count", "ux__dcd__ra8__usb__setup_8c.html#a27b99075886f8a3f8c0d2ec97420eca5", null ],
    [ "g_state_at_dispatch", "ux__dcd__ra8__usb__setup_8c.html#a4aefabab51260d4621e98c4e082792f2", null ],
    [ "s_dcd_dwt_cyccnt", "ux__dcd__ra8__usb__setup_8c.html#a4a47bcfed13354b791b7ba1525834593", null ],
    [ "s_trace", "ux__dcd__ra8__usb__setup_8c.html#a73ceae41aa620981bab0af846f123c47", null ],
    [ "s_trace_seq", "ux__dcd__ra8__usb__setup_8c.html#af3645b99c51e909c6bc750bc59c2e947", null ],
    [ "s_trace_ts", "ux__dcd__ra8__usb__setup_8c.html#a501b4613b074a41bde4da2bbc0f4c933", null ]
];