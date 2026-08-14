/**
 * @file test_ra8_c6link_mdl.c
 * @brief Generated-code and service-state tests for media download RPC.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_c6link_mdl.h"
#include "ra8_c6link_mdl_msg.h"
#include "ra8_media_download.pb-c.h"
#include "unity_minimal.h"

typedef struct {
  const uint8_t* bytes;
  size_t         len;
  size_t         at;
  uint32_t       begins;
  uint32_t       cancels;
} fake_backend_t;

static fake_backend_t    s_backend;
static ra8_mdl_service_t s_service;
static uint8_t           s_request[700];
static uint8_t           s_response[1200];

static ra8_err_t fake_begin(void* ctx, const char* url)
{
  fake_backend_t* fake = (fake_backend_t*)ctx;
  if (strcmp(url, "https://example.test/book") != 0) {
    return k_ra8_err_invalid_arg;
  }
  fake->at = 0U;
  fake->begins += 1U;
  return k_ra8_ok;
}

static ra8_err_t fake_read(void*     ctx,
                           uint8_t*  out,
                           uint16_t  cap,
                           uint16_t* got,
                           uint64_t* total_bytes,
                           bool*     complete,
                           uint8_t   sha256[k_ra8_mdl_sha256_bytes])
{
  fake_backend_t* fake = (fake_backend_t*)ctx;
  const size_t    left = fake->len - fake->at;
  const size_t    take = (left < cap) ? left : cap;
  if (take != 0U) {
    memcpy(out, &fake->bytes[fake->at], take);
    fake->at += take;
  }
  *got         = (uint16_t)take;
  *total_bytes = fake->len;
  *complete    = (take == 0U);
  if (*complete) {
    memset(sha256, 0xA5, k_ra8_mdl_sha256_bytes);
  }
  return k_ra8_ok;
}

static ra8_err_t fake_cancel(void* ctx)
{
  fake_backend_t* fake = (fake_backend_t*)ctx;
  fake->cancels += 1U;
  return k_ra8_ok;
}

static void reset_service(void)
{
  static const uint8_t bytes[]            = {'a', 'b', 'c', 'd', 'e', 'f'};
  s_backend                               = (fake_backend_t){.bytes = bytes, .len = sizeof(bytes)};
  const ra8_mdl_service_backend_t backend = {.begin  = fake_begin,
                                             .read   = fake_read,
                                             .cancel = fake_cancel,
                                             .ctx    = &s_backend};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_service_init(&s_service, &backend));
}

static uint32_t dispatch_start(void)
{
  Ra8__Mdl__StartRequest req = RA8__MDL__START_REQUEST__INIT;
  req.protocol_version       = k_ra8_mdl_protocol_version;
  req.url                    = (char*)"https://example.test/book";
  const size_t request_len   = ra8__mdl__start_request__pack(&req, s_request);
  size_t       response_len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  Ra8__Mdl__Accepted* accepted = ra8__mdl__accepted__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(accepted != nullptr);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_protocol_version, (int64_t)accepted->protocol_version);
  TEST_ASSERT(accepted->job_id != 0U);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_chunk_data_max, (int64_t)accepted->max_chunk_bytes);
  const uint32_t job = accepted->job_id;
  ra8__mdl__accepted__free_unpacked(accepted, nullptr);
  return job;
}

static Ra8__Mdl__Chunk* dispatch_next(uint32_t job, uint64_t offset, uint32_t max_bytes)
{
  Ra8__Mdl__NextRequest req = RA8__MDL__NEXT_REQUEST__INIT;
  req.protocol_version      = k_ra8_mdl_protocol_version;
  req.job_id                = job;
  req.acknowledged_offset   = offset;
  req.max_bytes             = max_bytes;
  const size_t request_len  = ra8__mdl__next_request__pack(&req, s_request);
  size_t       response_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  return ra8__mdl__chunk__unpack(nullptr, response_len, s_response);
}

static void test_service_multichunk_and_digest(void)
{
  TEST_BEGIN("mdl protobuf service multi-chunk");
  reset_service();
  const uint32_t job = dispatch_start();
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.begins);

  Ra8__Mdl__Chunk* first = dispatch_next(job, 0U, 4U);
  TEST_ASSERT(first != nullptr);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)first->sequence);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)first->offset);
  TEST_ASSERT_EQ((int64_t)4, (int64_t)first->data.len);
  TEST_ASSERT(memcmp(first->data.data, "abcd", 4U) == 0);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_DOWNLOADING, (int64_t)first->state);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)first->sha256.len);
  ra8__mdl__chunk__free_unpacked(first, nullptr);

  Ra8__Mdl__Chunk* second = dispatch_next(job, 4U, 4U);
  TEST_ASSERT(second != nullptr);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)second->sequence);
  TEST_ASSERT_EQ((int64_t)4, (int64_t)second->offset);
  TEST_ASSERT_EQ((int64_t)2, (int64_t)second->data.len);
  TEST_ASSERT(memcmp(second->data.data, "ef", 2U) == 0);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_DOWNLOADING, (int64_t)second->state);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)second->sha256.len);
  ra8__mdl__chunk__free_unpacked(second, nullptr);

  Ra8__Mdl__Chunk* terminal = dispatch_next(job, 6U, 4U);
  TEST_ASSERT(terminal != nullptr);
  TEST_ASSERT_EQ((int64_t)2, (int64_t)terminal->sequence);
  TEST_ASSERT_EQ((int64_t)6, (int64_t)terminal->offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)terminal->data.len);
  TEST_ASSERT_EQ((int64_t)RA8__MDL__STATE__STATE_COMPLETE, (int64_t)terminal->state);
  TEST_ASSERT_EQ((int64_t)k_ra8_mdl_sha256_bytes, (int64_t)terminal->sha256.len);
  for (size_t i = 0U; i < terminal->sha256.len; ++i) {
    TEST_ASSERT_EQ((int64_t)0xA5, (int64_t)terminal->sha256.data[i]);
  }
  ra8__mdl__chunk__free_unpacked(terminal, nullptr);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl protobuf service multi-chunk");
}

static void test_service_busy_stale_and_cancel(void)
{
  TEST_BEGIN("mdl service busy stale cancel");
  reset_service();
  const uint32_t job = dispatch_start();

  Ra8__Mdl__StartRequest second = RA8__MDL__START_REQUEST__INIT;
  second.protocol_version       = k_ra8_mdl_protocol_version;
  second.url                    = (char*)"https://example.test/book";
  size_t request_len            = ra8__mdl__start_request__pack(&second, s_request);
  size_t response_len           = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));

  Ra8__Mdl__NextRequest stale = RA8__MDL__NEXT_REQUEST__INIT;
  stale.protocol_version      = k_ra8_mdl_protocol_version;
  stale.job_id                = job + 1U;
  stale.max_bytes             = 4U;
  request_len                 = ra8__mdl__next_request__pack(&stale, s_request);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));

  Ra8__Mdl__CancelRequest cancel = RA8__MDL__CANCEL_REQUEST__INIT;
  cancel.protocol_version        = k_ra8_mdl_protocol_version;
  cancel.job_id                  = job;
  request_len                    = ra8__mdl__cancel_request__pack(&cancel, s_request);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  Ra8__Mdl__Cancelled* ack = ra8__mdl__cancelled__unpack(nullptr, response_len, s_response);
  TEST_ASSERT(ack != nullptr);
  TEST_ASSERT_EQ((int64_t)job, (int64_t)ack->job_id);
  ra8__mdl__cancelled__free_unpacked(ack, nullptr);
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.cancels);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl service busy stale cancel");
}

static void test_response_capacity_is_transactional(void)
{
  TEST_BEGIN("mdl response capacity is transactional");
  reset_service();

  Ra8__Mdl__StartRequest start = RA8__MDL__START_REQUEST__INIT;
  start.protocol_version       = k_ra8_mdl_protocol_version;
  start.url                    = (char*)"https://example.test/book";
  size_t request_len           = ra8__mdl__start_request__pack(&start, s_request);
  size_t response_len          = 99U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          request_len,
                                          s_response,
                                          1U,
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.begins);
  TEST_ASSERT(!s_service.active);

  const uint32_t job = dispatch_start();
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.begins);

  Ra8__Mdl__NextRequest next = RA8__MDL__NEXT_REQUEST__INIT;
  next.protocol_version      = k_ra8_mdl_protocol_version;
  next.job_id                = job;
  next.acknowledged_offset   = 0U;
  next.max_bytes             = 4U;
  request_len                = ra8__mdl__next_request__pack(&next, s_request);
  response_len               = 99U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_next,
                                          s_request,
                                          request_len,
                                          s_response,
                                          1U,
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.at);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_service.next_offset);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_service.next_sequence);

  Ra8__Mdl__Chunk* first = dispatch_next(job, 0U, 4U);
  TEST_ASSERT(first != nullptr);
  TEST_ASSERT_EQ((int64_t)4, (int64_t)first->data.len);
  TEST_ASSERT(memcmp(first->data.data, "abcd", 4U) == 0);
  ra8__mdl__chunk__free_unpacked(first, nullptr);

  Ra8__Mdl__CancelRequest cancel = RA8__MDL__CANCEL_REQUEST__INIT;
  cancel.protocol_version        = k_ra8_mdl_protocol_version;
  cancel.job_id                  = job;
  request_len                    = ra8__mdl__cancel_request__pack(&cancel, s_request);
  response_len                   = 99U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          1U,
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.cancels);
  TEST_ASSERT(s_service.active);

  response_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_cancel,
                                          s_request,
                                          request_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)1, (int64_t)s_backend.cancels);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl response capacity is transactional");
}

static void test_rejects_malformed(void)
{
  TEST_BEGIN("mdl rejects malformed");
  reset_service();
  size_t response_len = 99U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mdl_service_dispatch(&s_service,
                                          0xDEADBEEFU,
                                          (const uint8_t*)"x",
                                          1U,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          (const uint8_t*)"not protobuf",
                                          strlen("not protobuf"),
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  Ra8__Mdl__StartRequest insecure = RA8__MDL__START_REQUEST__INIT;
  insecure.protocol_version       = k_ra8_mdl_protocol_version;
  insecure.url                    = (char*)"http://example.test/book";
  const size_t insecure_len       = ra8__mdl__start_request__pack(&insecure, s_request);
  response_len                    = 99U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mdl_service_dispatch(&s_service,
                                          k_ra8_mdl_rpc_start,
                                          s_request,
                                          insecure_len,
                                          s_response,
                                          sizeof(s_response),
                                          &response_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)response_len);
  TEST_ASSERT_EQ((int64_t)0, (int64_t)s_backend.begins);
  TEST_ASSERT(!s_service.active);
  TEST_END("mdl rejects malformed");
}

int32_t main(void)
{
  test_service_multichunk_and_digest();
  test_service_busy_stale_and_cancel();
  test_response_capacity_is_transactional();
  test_rejects_malformed();
  (void)fprintf(stderr, "[OK  ] test_ra8_c6link_mdl.c\n");
  return 0;
}
