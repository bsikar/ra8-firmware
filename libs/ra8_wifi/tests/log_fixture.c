/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

#include "log_fixture.h"

#include <stddef.h>

#include "ra8_attributes.h"

static char   s_last_log[64];
static size_t s_last_log_len;

void ra8_log_emit_error(const char* tag, const char* message)
{
  (void)tag;
  s_last_log_len = 0U;
  if (message != NULL) {
    while (s_last_log_len < sizeof(s_last_log) && message[s_last_log_len] != '\0') {
      ++s_last_log_len;
    }
  }
  if (s_last_log_len != 0U) {
    for (size_t index = 0U; index < s_last_log_len; ++index) {
      s_last_log[index] = message[index];
    }
  }
}

RA8_TEST_HELPER void ra8_wifi_test_reset_log(void)
{
  s_last_log_len = 0U;
}

RA8_TEST_HELPER const unsigned char* ra8_wifi_test_last_log(void)
{
  return (const unsigned char*)s_last_log;
}

RA8_TEST_HELPER size_t ra8_wifi_test_last_log_len(void)
{
  return s_last_log_len;
}
