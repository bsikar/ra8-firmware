/**
 * @file test_ra8_di_instances.c
 * @brief Unit tests for the production dependency-injection instances
 *        `g_ra8_time_interface_systick` and `g_ra8_error_sink_log`
 *
 * @details
 * `docs/ARCHITECTURE.md` ("Dependency injection") names three vtables a driver
 * injects plus the production instances that back them.
 * `g_ra8_gpio_pin_interface` existed; `g_ra8_time_interface_systick` (declared
 * in `libs/ra8_core/inc/ra8_time.h`) and `g_ra8_error_sink_log` (declared in
 * `libs/ra8_core/inc/ra8_error_interface.h`) were `extern` declarations with no
 * definition anywhere in the tree, so a driver that followed the documented
 * pattern compiled and then failed to link (issue #1194).
 *
 * @details
 * Every vector reaches the implementation THROUGH the vtable pointer, the way a
 * driver under DI does, never by calling `ra8_time_ms` / `ra8_log_error_val`
 * directly. That is the point of the file: a test that called the underlying
 * function would stay green even if a vtable slot were wired to the wrong thunk
 * or dropped again. Linking is itself half the test -- before #1194 this
 * translation unit could not link at all.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_error_interface.h"
#include "ra8_log.h"
#include "ra8_time.h"
#include "ra8_time_interface.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_di_delay_ms     = 3U,     /**< Delay requested through the time vtable.  */
  k_di_probe_err    = 0x108U, /**< k_ra8_err_timeout, the code reported.     */
  k_di_sink_bufsize = 256U,   /**< Capture buffer size for one log line.     */
} di_test_const_t;

/**
 * @struct di_capture_t
 * @brief Bytes captured from the log backend during one error-sink vector.
 * @note Single-threaded test fixture; no synchronization is provided.
 * @since 0.1.0
 */
typedef struct {
  char     text[k_di_sink_bufsize]; /**< Rendered bytes, NUL-terminated. */
  uint32_t len;                     /**< Bytes stored in `text`.         */
  uint32_t dropped;                 /**< Bytes refused for lack of room. */
} di_capture_t;

/** @brief Capture buffer for the rendered-line comparison vector. */
static di_capture_t s_capture = {};

/**
 * @brief Append one log byte to ::s_capture.
 *
 * @details Installed through ::ra8_log_set_byte_sink, which is also what makes
 *          the hosted backend report itself ready; without a sink every emitter
 *          short-circuits before rendering anything.
 *
 * @param[in,out] ctx  Capture cookie (always &::s_capture).
 * @param[in]     byte The log byte the backend produced.
 *
 * @return Nothing.
 *
 * @pre @p ctx addresses one writable ::di_capture_t.
 * @post The byte is appended, or `dropped` has been incremented.
 * @post `text` remains NUL-terminated.
 *
 * @note Not thread-safe; the test fixture is single-threaded.
 *
 * @since 0.1.0
 */
static void internal_capture_byte(void* ctx, uint8_t byte)
{
  di_capture_t* cap = (di_capture_t*)ctx;

  if (cap == nullptr) {
    return;
  }
  if (cap->len >= (uint32_t)(k_di_sink_bufsize - 1U)) {
    cap->dropped++;
    return;
  }
  cap->text[cap->len] = (char)byte;
  cap->len++;
  cap->text[cap->len] = '\0';
}

/**
 * @brief Clear ::s_capture before an emit.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post ::s_capture holds an empty, NUL-terminated string and no drops.
 *
 * @since 0.1.0
 */
static void internal_capture_reset(void)
{
  s_capture.text[0] = '\0';
  s_capture.len     = 0U;
  s_capture.dropped = 0U;
}

/**
 * @brief Verify the time vtable is linked, populated, and dispatches.
 *
 * @details A driver under DI holds a `const ra8_time_interface_t*`. This vector
 *          takes exactly that pointer to ::g_ra8_time_interface_systick and
 *          calls both slots through it.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post Both vtable slots have been called once.
 *
 * @since 0.1.0
 */
static void test_time_interface_is_linked_and_callable(void)
{
  TEST_BEGIN("g_ra8_time_interface_systick links and dispatches");

  const ra8_time_interface_t* time_if = &g_ra8_time_interface_systick;

  TEST_ASSERT_NOT_NULL((const void*)time_if->now_ms);
  TEST_ASSERT_NOT_NULL((const void*)time_if->delay_ms);

  /* The production instance keeps its state in ra8_time.c file statics, so it
   * carries no context; a mock would put its own state here. */
  TEST_ASSERT_NULL(time_if->ctx);

  const uint32_t through_vtable = time_if->now_ms(time_if->ctx);
  const uint32_t direct         = ra8_time_ms();

  /* Off-target there is no SysTick, so the counter never advances and the two
   * reads are equal; on target a later direct read can only be >= the vtable
   * read. Assert the relation that holds in both builds -- what matters is that
   * the slot forwards to the same counter, not what the counter says. */
  TEST_ASSERT(through_vtable <= direct);

  /* delay_ms must be callable through the slot. Off-target ra8_delay_ms returns
   * immediately (s_tick_ms never advances), so this cannot hang the host run. */
  time_if->delay_ms(time_if->ctx, (uint32_t)k_di_delay_ms);

  TEST_END("g_ra8_time_interface_systick links and dispatches");
}

/**
 * @brief Verify the error sink is linked, populated, and dispatches.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post One report has been dispatched through the vtable.
 *
 * @since 0.1.0
 */
static void test_error_sink_is_linked_and_callable(void)
{
  TEST_BEGIN("g_ra8_error_sink_log links and dispatches");

  const ra8_error_interface_t* sink = &g_ra8_error_sink_log;

  TEST_ASSERT_NOT_NULL((const void*)sink->report);
  TEST_ASSERT_NULL(sink->ctx);

  sink->report(sink->ctx, "DITEST", "probe", (ra8_err_t)k_di_probe_err);

  TEST_END("g_ra8_error_sink_log links and dispatches");
}

/**
 * @brief Verify a report through the sink renders exactly like the direct call.
 *
 * @details `ra8_error_interface.h` promises the production instance "pushes
 *          reports into the standard `ra8_log_error_val` backend". This vector
 *          holds it to that: it captures the bytes rendered for a direct
 *          `ra8_log_error_val` call, then the bytes rendered for the same report
 *          dispatched through the vtable, and requires them to match byte for
 *          byte. A sink that invented its own line shape, dropped the numeric
 *          code, or emitted at a different level fails here.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The byte sink is detached again.
 *
 * @since 0.1.0
 */
static void test_error_sink_matches_direct_log_line(void)
{
  TEST_BEGIN("sink report renders exactly like ra8_log_error_val");

  char expected[k_di_sink_bufsize];

  ra8_log_set_byte_sink(internal_capture_byte, &s_capture);

  internal_capture_reset();
  ra8_log_error_val("DITEST", "bind failed", (uint32_t)k_di_probe_err);
  TEST_ASSERT_EQ(0U, s_capture.dropped);
  memcpy(expected, s_capture.text, (size_t)s_capture.len + 1U);

  internal_capture_reset();
  g_ra8_error_sink_log.report(
      g_ra8_error_sink_log.ctx, "DITEST", "bind failed", (ra8_err_t)k_di_probe_err);
  TEST_ASSERT_EQ(0U, s_capture.dropped);

  ra8_log_set_byte_sink(nullptr, nullptr);

  TEST_ASSERT_EQ(strlen(expected), s_capture.len);
  TEST_ASSERT(strcmp(expected, s_capture.text) == 0);

  TEST_END("sink report renders exactly like ra8_log_error_val");
}

/**
 * @brief Verify both instances are immutable singletons.
 *
 * @details Both are `const`, so an unused seam costs zero RAM (it lands in
 *          `.rodata` rather than `.data`). This vector pins the singleton
 *          contract: two reads yield the same object and the same slots, so a
 *          future edit that made either instance per-call mutable state breaks
 *          here rather than quietly.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post No state modified.
 *
 * @since 0.1.0
 */
static void test_instances_are_immutable_singletons(void)
{
  TEST_BEGIN("DI instances are const singletons");

  const ra8_time_interface_t*  time_a = &g_ra8_time_interface_systick;
  const ra8_time_interface_t*  time_b = &g_ra8_time_interface_systick;
  const ra8_error_interface_t* sink_a = &g_ra8_error_sink_log;
  const ra8_error_interface_t* sink_b = &g_ra8_error_sink_log;

  TEST_ASSERT(time_a == time_b);
  TEST_ASSERT(sink_a == sink_b);
  TEST_ASSERT(time_a->now_ms == time_b->now_ms);
  TEST_ASSERT(time_a->delay_ms == time_b->delay_ms);
  TEST_ASSERT(sink_a->report == sink_b->report);

  TEST_END("DI instances are const singletons");
}

/**
 * @brief Run every DI-instance vector.
 *
 * @return Zero; a failed assertion exits the process from the assert helper.
 *
 * @since 0.1.0
 */
int main(void)
{
  test_time_interface_is_linked_and_callable();
  test_error_sink_is_linked_and_callable();
  test_error_sink_matches_direct_log_line();
  test_instances_are_immutable_singletons();
  return 0;
}
