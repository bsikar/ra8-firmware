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

#include "ra8_audio.h"
#include "ra8_audio_source_memory.h"
#include "ra8_audio_source_pdm.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_t_audio_samples = 8U,     /**< PCM sample frames in the fixture. */
  k_t_audio_rate    = 16000U, /**< Fixture sample rate.              */
  k_t_audio_bytes   = 32U,    /**< Eight signed 32-bit samples.      */
} t_audio_const_t;

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
    .timestamp_ms   = 42U,
    .channels       = 1U,
    .valid_bits     = 20U,
    .format         = k_ra8_audio_format_pcm_s32le,
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
  frame.valid_bits = 33U;
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
    .poll_attempts     = 1U,
    .channel           = (uint8_t)k_ra8_pdm_ch_count,
    .valid_bits        = 20U,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_pdm_init(nullptr, &state, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  cfg.channel    = (uint8_t)k_ra8_pdm_ch2;
  cfg.valid_bits = 33U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  TEST_END("audio PDM config guards");
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
  internal_test_memory_source();
  internal_test_facade_guards();
  internal_test_pdm_config_guards();
}

int main(void)
{
  internal_test_mcdc_audio_sources();
  return 0;
}
