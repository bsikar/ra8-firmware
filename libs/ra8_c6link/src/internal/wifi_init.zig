//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `Req_WifiInit` configuration this host transmits.
//!
//! ESP-IDF's default set for an ESP32-C6, restated. The co-processor supplies
//! its own OS and crypto function tables, which are pointers and never on the
//! wire, and takes these scalars from the request. Nothing here is a decision
//! the host makes at run time: it is one validated set of numbers, which is
//! why it is constants and a filler rather than logic.
//!
//! `esp_wifi_init()` on the far side is what validates them. A wrong magic
//! word is refused with `ESP_ERR_INVALID_ARG` before anything else is read.

/// The numbers transmitted, each one documented where it is defined.
pub const Init = struct {
    /// `WIFI_INIT_CONFIG_MAGIC`; the first thing the far side validates.
    pub const magic: i32 = 0x1F2F3F4F;
    /// Static receive buffers.
    pub const static_rx: i32 = 10;
    /// Dynamic receive buffers.
    pub const dynamic_rx: i32 = 32;
    /// Transmit buffer type: dynamic, which is the IDF default.
    pub const tx_type: i32 = 1;
    /// Static transmit buffers; zero because the type above is dynamic.
    pub const static_tx: i32 = 0;
    /// Dynamic transmit buffers.
    pub const dynamic_tx: i32 = 32;
    /// Management receive buffers are static by default.
    pub const rx_mgmt_type: i32 = 0;
    /// Management receive buffers.
    pub const rx_mgmt_num: i32 = 5;
    /// Aggregation enabled in both directions, as IDF defaults it.
    pub const ampdu_on: i32 = 1;
    /// Let the co-processor persist calibration in its own NVS.
    pub const nvs_on: i32 = 1;
    /// Block-ack window.
    pub const ba_win: i32 = 6;
    /// Longest beacon the soft-AP path would build; unused by a station but
    /// part of the validated set.
    pub const beacon_max: i32 = 752;
    /// Management short-buffer count.
    pub const mgmt_sbuf: i32 = 32;
    /// Feature bitmap; bit zero is WPA3-SAE, which the bench network does not
    /// use but which costs nothing to advertise.
    pub const feature_caps: u64 = 1;
    /// ESP-NOW encrypted peer slots.
    pub const espnow_keys: i32 = 7;
    /// HE trigger-based queues.
    pub const hetb_queues: i32 = 3;
    /// Power-save while a station is disconnected, as IDF defaults it.
    pub const sta_disconnected_pm: i32 = 1;
};

/// The set as one object, in the shape `ra8_c6link_wifi.c` copies into the
/// generated `WifiInitConfig`.
///
/// The 64-bit field leads so the layout is the same on both sides without an
/// interior pad, and the one boolean travels as an `i32` rather than relying
/// on a C `bool`'s width.
pub const Cfg = extern struct {
    feature_caps: u64,
    static_rx_buf_num: i32,
    dynamic_rx_buf_num: i32,
    tx_buf_type: i32,
    static_tx_buf_num: i32,
    dynamic_tx_buf_num: i32,
    rx_mgmt_buf_type: i32,
    rx_mgmt_buf_num: i32,
    ampdu_rx_enable: i32,
    ampdu_tx_enable: i32,
    nvs_enable: i32,
    rx_ba_win: i32,
    beacon_max_len: i32,
    mgmt_sbuf_num: i32,
    sta_disconnected_pm: i32,
    espnow_max_encrypt_num: i32,
    tx_hetb_queue_num: i32,
    magic: i32,
};

/// The configuration this host sends, every field from `Init`.
pub fn cfg() Cfg {
    return .{
        .feature_caps = Init.feature_caps,
        .static_rx_buf_num = Init.static_rx,
        .dynamic_rx_buf_num = Init.dynamic_rx,
        .tx_buf_type = Init.tx_type,
        .static_tx_buf_num = Init.static_tx,
        .dynamic_tx_buf_num = Init.dynamic_tx,
        .rx_mgmt_buf_type = Init.rx_mgmt_type,
        .rx_mgmt_buf_num = Init.rx_mgmt_num,
        .ampdu_rx_enable = Init.ampdu_on,
        .ampdu_tx_enable = Init.ampdu_on,
        .nvs_enable = Init.nvs_on,
        .rx_ba_win = Init.ba_win,
        .beacon_max_len = Init.beacon_max,
        .mgmt_sbuf_num = Init.mgmt_sbuf,
        .sta_disconnected_pm = Init.sta_disconnected_pm,
        .espnow_max_encrypt_num = Init.espnow_keys,
        .tx_hetb_queue_num = Init.hetb_queues,
        .magic = Init.magic,
    };
}
