//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the `Req_WifiInit` configuration this host transmits.

const std = @import("std");
const wifi_init = @import("implementation").wifi_init;

test "the magic word is the one esp_wifi_init validates" {
    try std.testing.expectEqual(@as(i32, 0x1F2F3F4F), wifi_init.Init.magic);
    try std.testing.expectEqual(wifi_init.Init.magic, wifi_init.cfg().magic);
}

test "the receive buffer counts are the IDF defaults" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(@as(i32, 10), c.static_rx_buf_num);
    try std.testing.expectEqual(@as(i32, 32), c.dynamic_rx_buf_num);
}

test "transmit buffers are dynamic, so the static count is zero" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(@as(i32, 1), c.tx_buf_type);
    try std.testing.expectEqual(@as(i32, 0), c.static_tx_buf_num);
    try std.testing.expectEqual(@as(i32, 32), c.dynamic_tx_buf_num);
}

test "management receive buffers are static and counted" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(@as(i32, 0), c.rx_mgmt_buf_type);
    try std.testing.expectEqual(@as(i32, 5), c.rx_mgmt_buf_num);
    try std.testing.expectEqual(@as(i32, 32), c.mgmt_sbuf_num);
}

test "aggregation is enabled in both directions, from one constant" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(wifi_init.Init.ampdu_on, c.ampdu_rx_enable);
    try std.testing.expectEqual(c.ampdu_rx_enable, c.ampdu_tx_enable);
}

test "the co-processor keeps its own calibration" {
    try std.testing.expectEqual(@as(i32, 1), wifi_init.cfg().nvs_enable);
}

test "the block-ack window and beacon ceiling are the validated pair" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(@as(i32, 6), c.rx_ba_win);
    try std.testing.expectEqual(@as(i32, 752), c.beacon_max_len);
}

test "bit zero of the feature bitmap advertises WPA3-SAE" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(@as(u64, 1), c.feature_caps);
    try std.testing.expect(c.feature_caps & 1 != 0);
}

test "a disconnected station is allowed to power-save" {
    try std.testing.expectEqual(@as(i32, 1), wifi_init.cfg().sta_disconnected_pm);
}

test "the espnow and HE queue counts travel as sent" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(@as(i32, 7), c.espnow_max_encrypt_num);
    try std.testing.expectEqual(@as(i32, 3), c.tx_hetb_queue_num);
}

test "every field comes from Init, none left at zero by accident" {
    const c = wifi_init.cfg();
    try std.testing.expectEqual(wifi_init.Init.static_rx, c.static_rx_buf_num);
    try std.testing.expectEqual(wifi_init.Init.dynamic_rx, c.dynamic_rx_buf_num);
    try std.testing.expectEqual(wifi_init.Init.tx_type, c.tx_buf_type);
    try std.testing.expectEqual(wifi_init.Init.static_tx, c.static_tx_buf_num);
    try std.testing.expectEqual(wifi_init.Init.dynamic_tx, c.dynamic_tx_buf_num);
    try std.testing.expectEqual(wifi_init.Init.rx_mgmt_type, c.rx_mgmt_buf_type);
    try std.testing.expectEqual(wifi_init.Init.rx_mgmt_num, c.rx_mgmt_buf_num);
    try std.testing.expectEqual(wifi_init.Init.nvs_on, c.nvs_enable);
    try std.testing.expectEqual(wifi_init.Init.ba_win, c.rx_ba_win);
    try std.testing.expectEqual(wifi_init.Init.beacon_max, c.beacon_max_len);
    try std.testing.expectEqual(wifi_init.Init.mgmt_sbuf, c.mgmt_sbuf_num);
    try std.testing.expectEqual(wifi_init.Init.feature_caps, c.feature_caps);
    try std.testing.expectEqual(wifi_init.Init.espnow_keys, c.espnow_max_encrypt_num);
    try std.testing.expectEqual(wifi_init.Init.hetb_queues, c.tx_hetb_queue_num);
    try std.testing.expectEqual(wifi_init.Init.sta_disconnected_pm, c.sta_disconnected_pm);
}

test "the shape C reads is the 64-bit field then seventeen words" {
    try std.testing.expectEqual(@as(usize, 8), @alignOf(wifi_init.Cfg));
    // 8 for the leading u64 plus 17 words is 76, which the 8-byte alignment
    // rounds up to 80 with a trailing pad. No interior pad, which is the
    // point of leading with the wide field: C lays it out the same way.
    try std.testing.expectEqual(@as(usize, 80), @sizeOf(wifi_init.Cfg));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(wifi_init.Cfg, "feature_caps"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(wifi_init.Cfg, "static_rx_buf_num"));
    try std.testing.expectEqual(@as(usize, 72), @offsetOf(wifi_init.Cfg, "magic"));
}

test "two calls agree, so the set is constant rather than built" {
    try std.testing.expectEqual(wifi_init.cfg(), wifi_init.cfg());
}
