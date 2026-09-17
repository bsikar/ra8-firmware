/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

#include <stdint.h>

typedef void (*ra8_test_eth_handler_t)(void* ctx, uint32_t status_mask);

static uint16_t               s_eth_init_result;
static uint16_t               s_eth_deinit_result;
static uint32_t               s_eth_init_calls;
static uint32_t               s_eth_deinit_calls;
static uint32_t               s_eth_attach_calls;
static ra8_test_eth_handler_t s_attached_handler;
static uint32_t               s_log_error_calls;
static uint32_t               s_log_info_calls;
static uint32_t               s_log_error_val_calls;
static const char             s_empty_message[]        = "";
static const char*            s_log_last_error_message = s_empty_message;
static uint32_t               s_log_last_error_value;

uint16_t ra8_eth_init(void)
{
  ++s_eth_init_calls;
  return s_eth_init_result;
}

uint16_t ra8_eth_deinit(void)
{
  ++s_eth_deinit_calls;
  return s_eth_deinit_result;
}

void ra8_eth_attach_handler(ra8_test_eth_handler_t handler, void* ctx)
{
  ++s_eth_attach_calls;
  s_attached_handler = handler;
  (void)ctx;
}

void ra8_log_emit_error(const char* tag, const char* message)
{
  (void)tag;
  ++s_log_error_calls;
  s_log_last_error_message = message;
}

void ra8_log_emit_info(const char* tag, const char* message)
{
  (void)tag;
  (void)message;
  ++s_log_info_calls;
}

void ra8_log_emit_error_val(const char* tag, const char* message, uint32_t value)
{
  (void)tag;
  ++s_log_error_val_calls;
  s_log_last_error_message = message;
  s_log_last_error_value   = value;
}

void ra8_test_fixture_reset(void)
{
  s_eth_init_result        = 0U;
  s_eth_deinit_result      = 0U;
  s_eth_init_calls         = 0U;
  s_eth_deinit_calls       = 0U;
  s_eth_attach_calls       = 0U;
  s_attached_handler       = (ra8_test_eth_handler_t)0;
  s_log_error_calls        = 0U;
  s_log_info_calls         = 0U;
  s_log_error_val_calls    = 0U;
  s_log_last_error_message = s_empty_message;
  s_log_last_error_value   = 0U;
}

void ra8_test_set_eth_init_result(uint16_t result)
{
  s_eth_init_result = result;
}

void ra8_test_set_eth_deinit_result(uint16_t result)
{
  s_eth_deinit_result = result;
}

uint32_t ra8_test_eth_init_calls(void)
{
  return s_eth_init_calls;
}

uint32_t ra8_test_eth_deinit_calls(void)
{
  return s_eth_deinit_calls;
}

uint32_t ra8_test_eth_attach_calls(void)
{
  return s_eth_attach_calls;
}

ra8_test_eth_handler_t ra8_test_attached_handler(void)
{
  return s_attached_handler;
}

uint32_t ra8_test_log_error_calls(void)
{
  return s_log_error_calls;
}

uint32_t ra8_test_log_info_calls(void)
{
  return s_log_info_calls;
}

uint32_t ra8_test_log_error_val_calls(void)
{
  return s_log_error_val_calls;
}

const char* ra8_test_log_last_error_message(void)
{
  return s_log_last_error_message;
}

uint32_t ra8_test_log_last_error_value(void)
{
  return s_log_last_error_value;
}
