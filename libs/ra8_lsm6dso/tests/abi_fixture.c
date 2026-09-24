/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

#include <stdint.h>

static uint32_t    s_log_count;
static const char* s_log_last = "";

void ra8_log_emit_error(const char* tag, const char* message)
{
  (void)tag;
  ++s_log_count;
  s_log_last = message;
}

void ra8_test_fixture_reset(void)
{
  s_log_count = 0U;
  s_log_last  = "";
}

uint32_t ra8_test_log_count(void)
{
  return s_log_count;
}

const char* ra8_test_log_last(void)
{
  return s_log_last;
}
