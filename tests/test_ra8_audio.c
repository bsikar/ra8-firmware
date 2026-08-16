/**
 * @file test_ra8_audio.c
 * @brief Host tests for the generic audio source facade and replay backend.
 * @details Exercises PCM validation, caller-owned memory replay, facade guards,
 *          and PDM configuration rejection without physical audio hardware.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_audio.h"
#include "ra8_audio_internal.h"
#include "ra8_audio_source_memory.h"
#include "ra8_audio_source_pdm.h"
#include "unity_minimal.h"

/** @brief Fixture magnitudes shared by every audio vector in this file. */
typedef enum : uint32_t {
  k_t_audio_samples       = 8U,          /**< PCM sample frames in the fixture.       */
  k_t_audio_rate          = 16000U,      /**< Fixture sample rate.                    */
  k_t_audio_bytes         = 32U,         /**< Eight signed 32-bit samples.            */
  k_t_audio_timestamp     = 42U,         /**< Deterministic fixture timestamp.        */
  k_t_audio_s16_bytes     = 16U,         /**< Eight signed 16-bit mono samples.       */
  k_t_audio_short_bytes   = 28U,         /**< Capacity below one fixture frame.       */
  k_t_audio_short_samples = 7U,          /**< Sample count below the fixture frame.   */
  k_t_audio_half_rate     = 8000U,       /**< Sample rate below the fixture frame.    */
  k_t_audio_huge_samples  = 0x20000004U, /**< Byte product overflows to exactly 32.   */
  k_t_audio_zero          = 0U,          /**< Rejected zero-valued metadata field.    */
  k_t_audio_one_call      = 1U,          /**< Expected single backend dispatch count. */
  k_t_audio_two_calls     = 2U,          /**< Expected second backend dispatch count. */
} t_audio_const_t;

/** @brief Narrow fixture widths expressed in channels or significant bits. */
typedef enum : uint8_t {
  k_t_audio_mono       = 1U,  /**< Fixture interleaved channel count.          */
  k_t_audio_stereo     = 2U,  /**< Channel count differing from the fixture.   */
  k_t_audio_valid_bits = 20U, /**< Fixture significant bits per sample.        */
  k_t_audio_s16_bits   = 16U, /**< Significant bits of the s16le fixture.      */
  k_t_audio_wide_bits  = 24U, /**< Valid-bit width differing from the fixture. */
  k_t_audio_over_bits  = 33U, /**< Exceeds the 32-bit container width.         */
} t_audio_width_t;

/** @brief Injectable audio backend that drives every facade failure path. */
typedef struct {
  ra8_err_t                 info_err;      /**< Status returned by get_info.           */
  ra8_err_t                 capture_err;   /**< Status returned by capture.            */
  ra8_err_t                 stream_err;    /**< Status returned by stream_start.       */
  ra8_err_t                 stop_err;      /**< Status returned by stop.               */
  ra8_audio_source_info_t   info;          /**< Metadata reported by get_info.         */
  ra8_audio_frame_t         frame;         /**< Descriptor emitted by capture.         */
  bool                      alias_buffer;  /**< Emit the caller buffer as frame data.  */
  uint32_t                  stream_calls;  /**< Accepted stream_start dispatches.      */
  const ra8_audio_buffer_t* stream_buffer; /**< Buffer observed by stream_start.       */
  void*                     stream_ctx;    /**< Callback context seen by stream_start. */
} t_audio_fake_t;

/**
 * @brief Accept a frame callback without changing fixture state.
 * @details Supplies a valid callback target for facade guard-path tests.
 * @param[in,out] ctx Opaque callback context, unused by this fixture.
 * @param[in] frame Borrowed frame descriptor, unused by this fixture.
 * @pre The test owns any object referenced by `ctx`.
 * @pre `frame` is null or remains valid for the callback duration.
 * @post No caller-owned storage is modified.
 * @post No callback state is retained.
 * @note This helper intentionally performs no assertions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_audio_callback(void* ctx, const ra8_audio_frame_t* frame)
{
  (void)ctx;
  (void)frame;
}

/**
 * @brief Build a valid PCM fixture descriptor.
 * @details Wraps eight caller-owned signed samples with deterministic test metadata.
 * @param[in] samples Sample storage retained by the calling test.
 * @return Complete audio frame descriptor.
 * @retval ra8_audio_frame_t Descriptor referencing `samples`.
 * @pre `samples` addresses at least `k_t_audio_samples` values.
 * @pre The sample storage outlives uses of the returned descriptor.
 * @post The sample storage is unchanged.
 * @post The returned descriptor reports the fixed fixture geometry.
 * @note The timestamp is arbitrary but deterministic.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_audio_frame_t internal_audio_fixture(const int32_t* samples)
{
  return (ra8_audio_frame_t){
    .data           = samples,
    .bytes          = (uint32_t)k_t_audio_bytes,
    .sample_count   = (uint32_t)k_t_audio_samples,
    .sample_rate_hz = (uint32_t)k_t_audio_rate,
    .timestamp_ms   = (uint32_t)k_t_audio_timestamp,
    .channels       = (uint8_t)k_t_audio_mono,
    .valid_bits     = (uint8_t)k_t_audio_valid_bits,
    .format         = k_ra8_audio_format_pcm_s32le,
  };
}

/**
 * @brief Report the injected metadata or the configured failure status.
 * @details Serves the fake backend's `get_info` row so facade metadata paths stay reachable.
 * @param[in,out] ctx Bound ::t_audio_fake_t controller.
 * @param[out] out_info Destination for the fake metadata.
 * @return Injected or successful status.
 * @retval k_ra8_ok Metadata was copied into @p out_info.
 * @retval other The status configured in the controller.
 * @pre `ctx` addresses a fake owned by the calling test. @pre `out_info` is writable.
 * @post A successful call fills @p out_info. @post An injected failure leaves it untouched.
 * @note Single-threaded fixture use only. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fake_get_info(void* ctx, ra8_audio_source_info_t* out_info)
{
  t_audio_fake_t* const fake = ctx;
  if (fake->info_err != k_ra8_ok) {
    return fake->info_err;
  }
  *out_info = fake->info;
  return k_ra8_ok;
}

/**
 * @brief Emit the injected frame descriptor or the configured failure status.
 * @details Optionally aliases the caller buffer so the facade's ownership check can be varied.
 * @param[in,out] ctx Bound ::t_audio_fake_t controller.
 * @param[in] buffer Caller-owned destination descriptor.
 * @param[out] out_frame Destination for the emitted frame descriptor.
 * @return Injected or successful status.
 * @retval k_ra8_ok The configured frame descriptor was emitted.
 * @retval other The status configured in the controller.
 * @pre `ctx` addresses a fake owned by the calling test. @pre `buffer` and `out_frame` are valid.
 * @post Success writes @p out_frame. @post An injected failure leaves @p out_frame untouched.
 * @note No sample bytes are copied; this fake exercises descriptor contracts only. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fake_capture(void* ctx, const ra8_audio_buffer_t* buffer, ra8_audio_frame_t* out_frame)
{
  t_audio_fake_t* const fake = ctx;
  if (fake->capture_err != k_ra8_ok) {
    return fake->capture_err;
  }
  *out_frame = fake->frame;
  if (fake->alias_buffer) {
    out_frame->data = buffer->data;
  }
  return k_ra8_ok;
}

/**
 * @brief Record a streaming dispatch and return the configured status.
 * @details Captures the forwarded buffer and callback context so dispatch can be asserted.
 * @param[in,out] ctx Bound ::t_audio_fake_t controller.
 * @param[in] buffer Buffer forwarded by the facade.
 * @param[in] callback Frame callback forwarded by the facade.
 * @param[in,out] callback_ctx Opaque callback context forwarded by the facade.
 * @return Injected status.
 * @retval k_ra8_ok Streaming was accepted by the fake.
 * @retval other The status configured in the controller.
 * @pre `ctx` addresses a fake owned by the calling test. @pre The facade validated its arguments.
 * @post The dispatch counter increases by one. @post The forwarded buffer and context are stored.
 * @note The callback is never invoked by this fake. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fake_stream_start(void*                      ctx,
                                                         const ra8_audio_buffer_t*  buffer,
                                                         ra8_audio_frame_callback_t callback,
                                                         void*                      callback_ctx)
{
  t_audio_fake_t* const fake = ctx;
  (void)callback;
  fake->stream_buffer = buffer;
  fake->stream_ctx    = callback_ctx;
  fake->stream_calls++;
  return fake->stream_err;
}

/**
 * @brief Return the configured stop status without touching hardware.
 * @details Lets a test hold the facade handle bound by reporting a retryable stop failure.
 * @param[in,out] ctx Bound ::t_audio_fake_t controller.
 * @return Injected status.
 * @retval k_ra8_ok The fake accepted the stop request.
 * @retval other The status configured in the controller.
 * @pre `ctx` addresses a fake owned by the calling test. @pre The facade validated its arguments.
 * @post The controller is unchanged. @post No hardware state is touched.
 * @note Single-threaded fixture use only. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fake_stop(void* ctx)
{
  const t_audio_fake_t* const fake = ctx;
  return fake->stop_err;
}

/** @brief Fully bound fake vtable used by the facade dispatch vectors. */
static const ra8_audio_source_iface_t s_audio_fake_iface = {
  .get_info     = internal_fake_get_info,
  .capture      = internal_fake_capture,
  .stream_start = internal_fake_stream_start,
  .stop         = internal_fake_stop,
};

/** @brief Vtable carrying no operation at all, modelling an unbound backend. */
static const ra8_audio_source_iface_t s_audio_unbound_iface = {};

/**
 * @brief Build a fake controller whose metadata matches the PCM fixture exactly.
 * @details Every injected status starts successful so a test perturbs exactly one field.
 * @param[in] samples Sample storage retained by the calling test.
 * @return Fully populated fake controller.
 * @retval t_audio_fake_t Controller describing the standard fixture geometry.
 * @pre `samples` addresses at least `k_t_audio_samples` values.
 * @pre The sample storage outlives every use of the controller.
 * @post The returned metadata matches ::internal_audio_fixture field for field.
 * @post No injected failure is armed.
 * @note The caller owns the returned controller. @since 0.1.0
 */
RA8_INTERNAL static t_audio_fake_t internal_audio_fake_new(const int32_t* samples)
{
  return (t_audio_fake_t){
    .info =
      (ra8_audio_source_info_t){
        .frame_bytes       = (uint32_t)k_t_audio_bytes,
        .samples_per_frame = (uint32_t)k_t_audio_samples,
        .sample_rate_hz    = (uint32_t)k_t_audio_rate,
        .channels          = (uint8_t)k_t_audio_mono,
        .valid_bits        = (uint8_t)k_t_audio_valid_bits,
        .format            = k_ra8_audio_format_pcm_s32le,
      },
    .frame        = internal_audio_fixture(samples),
    .alias_buffer = true,
  };
}

/**
 * @brief Verify PCM frame validation guards.
 * @details Exercises valid geometry, null pointers, invalid widths, sizes, and formats.
 * @par MC/DC:
 * The valid frame is the all-false rejection baseline. Each subsequent vector
 * makes one documented rejection condition true while restoring all others.
 * @pre Unity test accounting is initialized.
 * @pre Stack storage is available for the fixture samples.
 * @post Every validation branch records its expected result.
 * @post No persistent fixture state remains.
 * @note This test does not access audio hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_frame_validation(void)
{
  TEST_BEGIN("audio frame validation");
  int32_t           samples[k_t_audio_samples] = {};
  ra8_audio_frame_t frame                      = internal_audio_fixture(samples);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_frame_validate(&frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_frame_validate(nullptr));
  frame.data = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_frame_validate(&frame));
  frame            = internal_audio_fixture(samples);
  frame.valid_bits = (uint8_t)k_t_audio_over_bits;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_frame_validate(&frame));
  frame       = internal_audio_fixture(samples);
  frame.bytes = (uint32_t)k_t_audio_bytes - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_audio_frame_validate(&frame));
  frame        = internal_audio_fixture(samples);
  frame.format = (ra8_audio_format_t)99U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_frame_validate(&frame));
  TEST_END("audio frame validation");
}

/**
 * @brief Verify fixed-memory source capture and stop behavior.
 * @details Checks metadata, byte-for-byte replay, unsupported streaming, and handle clearing.
 * @par MC/DC:
 * The accepted capture is the all-false rejection baseline; the remaining
 * vectors independently vary destination capacity, stream support, and handle state.
 * @pre Unity test accounting is initialized.
 * @pre Stack storage is available for source and destination fixtures.
 * @post Captured samples equal the immutable source fixture.
 * @post The source handle is cleared after a successful stop.
 * @note This test exercises only caller-owned memory.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_memory_source(void)
{
  TEST_BEGIN("audio memory source");
  int32_t                         source_samples[k_t_audio_samples]  = {1, -2, 3, -4, 5, -6, 7, -8};
  int32_t                         capture_samples[k_t_audio_samples] = {};
  const ra8_audio_frame_t         frame  = internal_audio_fixture(source_samples);
  ra8_audio_source_t              source = {};
  ra8_audio_source_memory_state_t state  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_memory_init(&source, &state, &frame));
  ra8_audio_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_t_audio_bytes, info.frame_bytes);
  TEST_ASSERT_EQ(k_t_audio_samples, info.samples_per_frame);
  const ra8_audio_buffer_t buffer = {
    .data     = capture_samples,
    .capacity = (uint32_t)sizeof(capture_samples),
  };
  ra8_audio_frame_t captured = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_capture(&source, &buffer, &captured));
  TEST_ASSERT(captured.data == capture_samples);
  TEST_ASSERT(memcmp(source_samples, capture_samples, sizeof(source_samples)) == 0);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_audio_source_stream_start(&source, &buffer, internal_audio_callback, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_stop(&source));
  TEST_END("audio memory source");
}

/**
 * @brief Verify generic audio facade argument guards.
 * @details Exercises uninitialized sources and null arguments without invoking a backend.
 * @par MC/DC:
 * Each vector makes exactly one facade dependency absent while the companion
 * arguments remain valid; the initialized fixture supplies the accepted baseline.
 * @pre Unity test accounting is initialized.
 * @pre Local descriptors begin zero-initialized.
 * @post Each invalid call records the documented error.
 * @post No hardware operation is attempted.
 * @note The callback is supplied only to reach stream-dispatch guards.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_facade_guards(void)
{
  TEST_BEGIN("audio facade guards");
  int32_t                  samples[k_t_audio_samples] = {};
  const ra8_audio_buffer_t buffer                     = {
    .data     = samples,
    .capacity = (uint32_t)sizeof(samples),
  };
  ra8_audio_source_t      source = {};
  ra8_audio_source_info_t info   = {};
  ra8_audio_frame_t       frame  = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_capture(&source, &buffer, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_get_info(nullptr, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_capture(&source, nullptr, &frame));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_audio_source_stream_start(&source, &buffer, internal_audio_callback, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_stop(nullptr));
  TEST_END("audio facade guards");
}

/**
 * @brief Verify PDM source configuration rejection.
 * @details Checks null state, out-of-range channel, and invalid valid-bit width paths.
 * @par MC/DC:
 * The valid PDM configuration is the all-false rejection baseline. The null
 * state, invalid channel, and invalid width vectors flip one guard each.
 * @pre Unity test accounting is initialized.
 * @pre The configuration object is writable by the test.
 * @post Each malformed configuration records its expected error.
 * @post PDM hardware remains uninitialized.
 * @note This test deliberately avoids physical audio hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pdm_config_guards(void)
{
  TEST_BEGIN("audio PDM config guards");
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  ra8_audio_source_pdm_cfg_t   cfg    = {
    .sample_rate_hz    = (uint32_t)k_t_audio_rate,
    .samples_per_frame = (uint32_t)k_t_audio_samples,
    .poll_attempts     = (uint32_t)k_t_audio_one_call,
    .channel           = (uint8_t)k_ra8_pdm_ch_count,
    .valid_bits        = (uint8_t)k_t_audio_valid_bits,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_pdm_init(nullptr, &state, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  cfg.channel    = (uint8_t)k_ra8_pdm_ch2;
  cfg.valid_bits = (uint8_t)k_t_audio_over_bits;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  TEST_END("audio PDM config guards");
}

/**
 * @brief Verify the remaining scalar rejections of PCM frame validation.
 * @details Accepts the 16-bit container width, then zeroes each scalar metadata field in
 *          turn and finally overflows the 32-bit byte product.
 * @pre Unity test accounting is initialized.
 * @pre Stack storage is available for both sample fixtures.
 * @post Each zeroed scalar records `k_ra8_err_invalid_arg`.
 * @post The overflowing geometry records `k_ra8_err_invalid_size`.
 * @note The overflow vector wraps to exactly `k_t_audio_bytes`, so only the 64-bit range
 *       check can reject it; it never dereferences its sample storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_frame_validate_scalars(void)
{
  TEST_BEGIN("audio frame scalar validation");
  int32_t           samples[k_t_audio_samples] = {};
  int16_t           narrow[k_t_audio_samples]  = {};
  ra8_audio_frame_t frame                      = internal_audio_fixture(samples);
  frame.data                                   = narrow;
  frame.format                                 = k_ra8_audio_format_pcm_s16le;
  frame.bytes                                  = (uint32_t)k_t_audio_s16_bytes;
  frame.valid_bits                             = (uint8_t)k_t_audio_s16_bits;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_frame_validate(&frame));
  frame              = internal_audio_fixture(samples);
  frame.sample_count = (uint32_t)k_t_audio_zero;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_frame_validate(&frame));
  frame                = internal_audio_fixture(samples);
  frame.sample_rate_hz = (uint32_t)k_t_audio_zero;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_frame_validate(&frame));
  frame          = internal_audio_fixture(samples);
  frame.channels = (uint8_t)k_t_audio_zero;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_frame_validate(&frame));
  frame            = internal_audio_fixture(samples);
  frame.valid_bits = (uint8_t)k_t_audio_zero;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_frame_validate(&frame));
  frame              = internal_audio_fixture(samples);
  frame.sample_count = (uint32_t)k_t_audio_huge_samples;
  frame.channels     = (uint8_t)k_t_audio_stereo;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_audio_frame_validate(&frame));
  TEST_END("audio frame scalar validation");
}

/**
 * @brief Verify the capture and stop argument guards of the facade.
 * @details Drives each rejected argument shape against a bound fake and an operation-less vtable.
 * @pre Unity test accounting is initialized.
 * @pre The fake controller and its sample storage are stack-owned.
 * @post Every rejected call records its documented error code.
 * @post No rejected call reaches the fake backend.
 * @note The fake is armed to emit backend-owned storage, so a buffer guard that stopped
 *       rejecting would surface as `k_ra8_err_invalid_state` instead of the same code.
 * @note The unbound vtable models a handle whose backend rows are absent.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_capture_argument_guards(void)
{
  TEST_BEGIN("audio capture argument guards");
  int32_t                  samples[k_t_audio_samples] = {};
  t_audio_fake_t           fake                       = internal_audio_fake_new(samples);
  ra8_audio_source_t       source  = {.iface = &s_audio_fake_iface, .ctx = &fake};
  ra8_audio_source_t       unbound = {.iface = &s_audio_unbound_iface, .ctx = &fake};
  ra8_audio_source_info_t  info    = {};
  ra8_audio_frame_t        frame   = {};
  const ra8_audio_buffer_t good    = {.data = samples, .capacity = (uint32_t)sizeof(samples)};
  const ra8_audio_buffer_t no_data = {.data = nullptr, .capacity = (uint32_t)sizeof(samples)};
  const ra8_audio_buffer_t no_room = {.data = samples, .capacity = (uint32_t)k_t_audio_zero};
  fake.alias_buffer                = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_get_info(&source, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_get_info(&unbound, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_capture(nullptr, &good, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_capture(&source, &good, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_capture(&unbound, &good, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_capture(&source, &no_data, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_capture(&source, &no_room, &frame));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_stop(&unbound));
  TEST_ASSERT_NULL(frame.data);
  TEST_END("audio capture argument guards");
}

/**
 * @brief Verify the streaming argument guards of the facade.
 * @details Rejects a missing handle, buffer, callback, buffer storage, and metadata row in turn.
 * @pre Unity test accounting is initialized.
 * @pre The fake controller and its sample storage are stack-owned.
 * @post Every rejected call records its documented error code.
 * @post The fake backend records no streaming dispatch.
 * @note The callback exists only so the callback guard can be varied independently.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stream_argument_guards(void)
{
  TEST_BEGIN("audio stream argument guards");
  int32_t                  samples[k_t_audio_samples] = {};
  t_audio_fake_t           fake                       = internal_audio_fake_new(samples);
  ra8_audio_source_t       source  = {.iface = &s_audio_fake_iface, .ctx = &fake};
  ra8_audio_source_t       unbound = {.iface = &s_audio_unbound_iface, .ctx = &fake};
  const ra8_audio_buffer_t good    = {.data = samples, .capacity = (uint32_t)sizeof(samples)};
  const ra8_audio_buffer_t no_data = {.data = nullptr, .capacity = (uint32_t)sizeof(samples)};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_audio_source_stream_start(nullptr, &good, internal_audio_callback, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_audio_source_stream_start(&source, nullptr, internal_audio_callback, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_audio_source_stream_start(&source, &good, nullptr, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_audio_source_stream_start(&source, &no_data, internal_audio_callback, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_audio_source_stream_start(&unbound, &good, internal_audio_callback, nullptr));
  TEST_ASSERT_EQ(k_t_audio_zero, fake.stream_calls);
  TEST_END("audio stream argument guards");
}

/**
 * @brief Verify streaming dispatch, metadata failure, and capacity rejection.
 * @details Injects a metadata failure, starves the buffer, then asserts the accepted dispatch
 *          forwards the caller's buffer and context and propagates the backend status.
 * @pre Unity test accounting is initialized.
 * @pre The fake controller and its sample storage are stack-owned.
 * @post A metadata failure and an undersized buffer both bypass the backend.
 * @post An accepted start forwards the exact caller buffer and callback context.
 * @note The fake never invokes the frame callback.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stream_dispatch(void)
{
  TEST_BEGIN("audio stream dispatch");
  int32_t                  samples[k_t_audio_samples] = {};
  uint32_t                 token                      = (uint32_t)k_t_audio_timestamp;
  t_audio_fake_t           fake                       = internal_audio_fake_new(samples);
  ra8_audio_source_t       source = {.iface = &s_audio_fake_iface, .ctx = &fake};
  const ra8_audio_buffer_t good   = {.data = samples, .capacity = (uint32_t)sizeof(samples)};
  const ra8_audio_buffer_t small  = {.data = samples, .capacity = (uint32_t)k_t_audio_short_bytes};
  fake.info_err                   = k_ra8_err_hw_error;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_audio_source_stream_start(&source, &good, internal_audio_callback, &token));
  fake.info_err = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_audio_source_stream_start(&source, &small, internal_audio_callback, &token));
  TEST_ASSERT_EQ(k_t_audio_zero, fake.stream_calls);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_audio_source_stream_start(&source, &good, internal_audio_callback, &token));
  TEST_ASSERT_EQ(k_t_audio_one_call, fake.stream_calls);
  TEST_ASSERT(fake.stream_buffer == &good);
  TEST_ASSERT(fake.stream_ctx == &token);
  fake.stream_err = k_ra8_err_busy;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_audio_source_stream_start(&source, &good, internal_audio_callback, &token));
  TEST_ASSERT_EQ(k_t_audio_two_calls, fake.stream_calls);
  TEST_END("audio stream dispatch");
}

/**
 * @brief Verify capture error propagation and rejected-frame clearing.
 * @details Injects a metadata failure, an undersized buffer, a capture failure, a frame that
 *          does not alias the caller buffer, and a frame that fails PCM validation.
 * @pre Unity test accounting is initialized.
 * @pre The fake controller and both sample arrays are stack-owned.
 * @post Each rejected capture records the backend's own error code.
 * @post Each rejected capture leaves the output descriptor zeroed.
 * @note The alias vector proves the facade rejects backend-owned storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_capture_error_propagation(void)
{
  TEST_BEGIN("audio capture error propagation");
  int32_t                  source_samples[k_t_audio_samples]  = {};
  int32_t                  capture_samples[k_t_audio_samples] = {};
  t_audio_fake_t           fake   = internal_audio_fake_new(source_samples);
  ra8_audio_source_t       source = {.iface = &s_audio_fake_iface, .ctx = &fake};
  ra8_audio_frame_t        frame  = {};
  const ra8_audio_buffer_t good   = {.data     = capture_samples,
                                     .capacity = (uint32_t)sizeof(capture_samples)};
  const ra8_audio_buffer_t small  = {.data     = capture_samples,
                                     .capacity = (uint32_t)k_t_audio_short_bytes};
  fake.info_err                   = k_ra8_err_hw_error;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_audio_source_capture(&source, &good, &frame));
  fake.info_err = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_audio_source_capture(&source, &small, &frame));
  fake.capture_err = k_ra8_err_timeout;
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_audio_source_capture(&source, &good, &frame));
  TEST_ASSERT_NULL(frame.data);
  TEST_ASSERT_EQ(k_t_audio_zero, frame.bytes);
  fake.capture_err  = k_ra8_ok;
  fake.alias_buffer = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_capture(&source, &good, &frame));
  TEST_ASSERT_NULL(frame.data);
  fake.alias_buffer     = true;
  fake.frame.valid_bits = (uint8_t)k_t_audio_zero;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_capture(&source, &good, &frame));
  TEST_ASSERT_NULL(frame.data);
  TEST_END("audio capture error propagation");
}

/**
 * @brief Verify a captured frame must match every advertised metadata field.
 * @details Starts from a fully matching capture, then perturbs exactly one metadata field per
 *          vector so each comparison is shown to influence the verdict on its own.
 * @pre Unity test accounting is initialized.
 * @pre The fake controller and both sample arrays are stack-owned.
 * @post The matching baseline captures successfully.
 * @post Every single-field mismatch records `k_ra8_err_invalid_state`.
 * @note Each vector restores the perturbed field before the next one runs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_capture_info_mismatch(void)
{
  TEST_BEGIN("audio capture metadata mismatch");
  int32_t                  source_samples[k_t_audio_samples]  = {};
  int32_t                  capture_samples[k_t_audio_samples] = {};
  t_audio_fake_t           fake   = internal_audio_fake_new(source_samples);
  ra8_audio_source_t       source = {.iface = &s_audio_fake_iface, .ctx = &fake};
  ra8_audio_frame_t        frame  = {};
  const ra8_audio_buffer_t good   = {.data     = capture_samples,
                                     .capacity = (uint32_t)sizeof(capture_samples)};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_capture(&source, &good, &frame));
  TEST_ASSERT(frame.data == capture_samples);
  fake.info.frame_bytes = (uint32_t)k_t_audio_short_bytes;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_capture(&source, &good, &frame));
  fake.info.frame_bytes       = (uint32_t)k_t_audio_bytes;
  fake.info.samples_per_frame = (uint32_t)k_t_audio_short_samples;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_capture(&source, &good, &frame));
  fake.info.samples_per_frame = (uint32_t)k_t_audio_samples;
  fake.info.sample_rate_hz    = (uint32_t)k_t_audio_half_rate;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_capture(&source, &good, &frame));
  fake.info.sample_rate_hz = (uint32_t)k_t_audio_rate;
  fake.info.channels       = (uint8_t)k_t_audio_stereo;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_capture(&source, &good, &frame));
  fake.info.channels   = (uint8_t)k_t_audio_mono;
  fake.info.valid_bits = (uint8_t)k_t_audio_wide_bits;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_capture(&source, &good, &frame));
  fake.info.valid_bits = (uint8_t)k_t_audio_valid_bits;
  fake.info.format     = k_ra8_audio_format_pcm_s16le;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_capture(&source, &good, &frame));
  TEST_ASSERT_NULL(frame.data);
  TEST_END("audio capture metadata mismatch");
}

/**
 * @brief Verify stop propagates a backend failure and keeps the handle bound.
 * @details A refused stop must leave the handle usable for a retry; only success clears it.
 * @pre Unity test accounting is initialized.
 * @pre The fake controller and its sample storage are stack-owned.
 * @post A refused stop leaves both handle fields bound to the fake.
 * @post An accepted stop clears both handle fields.
 * @note The fake never touches hardware in either direction.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stop_dispatch(void)
{
  TEST_BEGIN("audio stop dispatch");
  int32_t            samples[k_t_audio_samples] = {};
  t_audio_fake_t     fake                       = internal_audio_fake_new(samples);
  ra8_audio_source_t source                     = {.iface = &s_audio_fake_iface, .ctx = &fake};
  fake.stop_err                                 = k_ra8_err_busy;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_audio_source_stop(&source));
  TEST_ASSERT(source.iface == &s_audio_fake_iface);
  TEST_ASSERT(source.ctx == &fake);
  fake.stop_err = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_ASSERT_NULL(source.iface);
  TEST_ASSERT_NULL(source.ctx);
  TEST_END("audio stop dispatch");
}

/**
 * @brief Verify the memory replay backend rejects malformed binding requests.
 * @details Drives each null argument and both frame-validation failures the binding propagates.
 * @pre Unity test accounting is initialized.
 * @pre The handle, state, and sample storage are stack-owned.
 * @post Each rejected binding records the exact propagated error code.
 * @post A rejected binding leaves the source handle unbound.
 * @note Propagation is asserted with two distinct validation errors, not one.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_memory_init_guards(void)
{
  TEST_BEGIN("audio memory init guards");
  int32_t                         samples[k_t_audio_samples] = {};
  ra8_audio_source_t              source                     = {};
  ra8_audio_source_memory_state_t state                      = {};
  ra8_audio_frame_t               frame                      = internal_audio_fixture(samples);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_memory_init(nullptr, &state, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_memory_init(&source, nullptr, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_memory_init(&source, &state, nullptr));
  frame.bytes = (uint32_t)k_t_audio_short_bytes;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_audio_source_memory_init(&source, &state, &frame));
  frame      = internal_audio_fixture(samples);
  frame.data = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_memory_init(&source, &state, &frame));
  TEST_ASSERT_NULL(source.iface);
  TEST_ASSERT_NULL(state.frame.data);
  TEST_END("audio memory init guards");
}

/**
 * @brief Verify the memory backend rows guard themselves independently of the facade.
 * @details Dispatches directly through the bound vtable because the facade rejects these
 *          argument shapes before any backend row is reached.
 * @pre Unity test accounting is initialized.
 * @pre The bound source, state, and both sample arrays are stack-owned.
 * @post Each rejected row call records its documented error code.
 * @post An accepted metadata call still reports the bound fixture geometry.
 * @note Direct dispatch is the only route to the backend's own argument guards.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_memory_backend_dispatch(void)
{
  TEST_BEGIN("audio memory backend dispatch");
  int32_t                         source_samples[k_t_audio_samples]  = {9, 8, 7, 6, 5, 4, 3, 2};
  int32_t                         capture_samples[k_t_audio_samples] = {};
  const ra8_audio_frame_t         fixture = internal_audio_fixture(source_samples);
  ra8_audio_source_t              source  = {};
  ra8_audio_source_memory_state_t state   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_memory_init(&source, &state, &fixture));
  const ra8_audio_source_iface_t* iface = source.iface;
  ra8_audio_source_info_t         info  = {};
  ra8_audio_frame_t               frame = {};
  const ra8_audio_buffer_t        good  = {.data     = capture_samples,
                                           .capacity = (uint32_t)sizeof(capture_samples)};
  const ra8_audio_buffer_t        small = {.data     = capture_samples,
                                           .capacity = (uint32_t)k_t_audio_short_bytes};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->get_info(nullptr, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->get_info(&state, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->capture(nullptr, &good, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->capture(&state, nullptr, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->capture(&state, &good, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, iface->capture(&state, &small, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->stop(nullptr));
  TEST_ASSERT_NULL(frame.data);
  TEST_ASSERT_EQ(k_ra8_ok, iface->get_info(&state, &info));
  TEST_ASSERT_EQ(k_t_audio_rate, info.sample_rate_hz);
  TEST_ASSERT_EQ(k_ra8_ok, iface->stop(&state));
  TEST_END("audio memory backend dispatch");
}

/**
 * @brief Run the generic audio and PDM-source MC/DC vectors.
 * @details Executes each focused fixture group exactly once so the target
 *          covers the facade, memory backend, and PDM validation decisions.
 * @par MC/DC:
 * Decisions: libs/ra8_audio/src/ra8_audio.c@ra8_audio_frame_validate,
 * libs/ra8_audio/src/ra8_audio.c@ra8_audio_source_capture,
 * libs/ra8_audio/src/ra8_audio.c@ra8_audio_source_get_info,
 * libs/ra8_audio/src/ra8_audio.c@ra8_audio_source_stop,
 * libs/ra8_audio/src/ra8_audio.c@ra8_audio_source_stream_start,
 * libs/ra8_audio/src/ra8_audio_source_memory.c@internal_memory_capture,
 * libs/ra8_audio/src/ra8_audio_source_memory.c@internal_memory_get_info,
 * libs/ra8_audio/src/ra8_audio_source_memory.c@ra8_audio_source_memory_init,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@internal_pdm_capture,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@internal_pdm_get_info,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@internal_pdm_prepare_hardware,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@internal_pdm_stop,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@internal_pdm_stream_data,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@internal_pdm_stream_start,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@internal_pdm_validate_cfg,
 * libs/ra8_audio/src/ra8_audio_source_pdm.c@ra8_audio_source_pdm_init.
 * @pre Unity test accounting is initialized.
 * @pre All fixture helpers are linked into this executable.
 * @post Every audio vector group has executed once.
 * @post No fixture state survives the function return.
 * @note Hardware-reaching PDM vectors stop at configuration guards.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_audio_sources(void)
{
  internal_test_frame_validation();
  internal_test_frame_validate_scalars();
  internal_test_memory_source();
  internal_test_memory_init_guards();
  internal_test_memory_backend_dispatch();
  internal_test_facade_guards();
  internal_test_capture_argument_guards();
  internal_test_capture_error_propagation();
  internal_test_capture_info_mismatch();
  internal_test_stream_argument_guards();
  internal_test_stream_dispatch();
  internal_test_stop_dispatch();
  internal_test_pdm_config_guards();
}

int main(void)
{
  internal_test_mcdc_audio_sources();
  return 0;
}
