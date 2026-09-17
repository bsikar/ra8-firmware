/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static size_t s_log_calls;
static char s_last_tag[64];
static char s_last_message[96];

void ra8_log_emit_error(const char* tag, const char* message) {
  ++s_log_calls;
  (void)snprintf(s_last_tag, sizeof(s_last_tag), "%s", tag);
  (void)snprintf(s_last_message, sizeof(s_last_message), "%s", message);
}

void ra8_ota_test_log_reset(void) {
  s_log_calls = 0;
  s_last_tag[0] = '\0';
  s_last_message[0] = '\0';
}

size_t ra8_ota_test_log_calls(void) { return s_log_calls; }
const char* ra8_ota_test_log_tag(void) { return s_last_tag; }
const char* ra8_ota_test_log_message(void) { return s_last_message; }
