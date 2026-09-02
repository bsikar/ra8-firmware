/**
 * @file src/sweep_block_payload.c
 * @brief Checkpointed allocation-free pseudo-text source for block sweeps.
 * @details Generates repeatable prose bytes from serialized checkpoints so
 *          arbitrary reads regenerate at most one bounded checkpoint span.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <string.h>

#include "sweep_block_internal.h"

typedef enum : uint32_t {
  k_cbs_payload_checkpoint_bytes = 16384U,     /**< Maximum seek regeneration.  */
  k_cbs_no_word                  = UINT32_MAX, /**< Generator needs a new word. */
} cbs_payload_limit_t;

typedef enum : uint8_t {
  k_cbs_phase_word = 0U, /**< Emit a word byte.       */
  k_cbs_phase_dot,       /**< Emit a sentence period. */
  k_cbs_phase_newline,   /**< Emit a line break.      */
  k_cbs_phase_space,     /**< Emit a word separator.  */
} cbs_payload_phase_t;

typedef enum : uint64_t {
  k_cbs_payload_seed = 0x9E3779B97F4A7C15ULL, /**< Initial generator state. */
} cbs_payload_seed_t;

typedef enum : uint8_t {
  k_cbs_payload_shift_a = 13U, /**< First xorshift distance.  */
  k_cbs_payload_shift_b = 7U,  /**< Middle xorshift distance. */
  k_cbs_payload_shift_c = 17U, /**< Final xorshift distance.  */
} cbs_payload_shift_t;

static const char* const s_payload_words[] = {
  "the",     "quick",   "reader", "turns",   "another", "page",    "while",   "morning",
  "light",   "settles", "across", "quiet",   "margins", "and",     "chapter", "headings",
  "gather",  "small",   "notes",  "between", "lines",   "of",      "steady",  "prose",
  "carried", "through", "paper",  "towns",   "by",      "patient", "hands",   "again",
};

/**
 * @brief Advance the deterministic payload pseudo-random generator.
 * @details Applies the fixed xorshift sequence and stores the new state for
 *          reproducible word selection at every checkpoint.
 * @param[in,out] state Non-zero generator state.
 * @return Updated 64-bit pseudo-random value.
 * @retval other Deterministic next value in the xorshift sequence.
 * @pre @p state is non-NULL and points to writable storage.
 * @pre The initial seed is non-zero.
 * @post `*state` equals the returned value.
 * @post No state outside @p state is modified.
 * @note This generator is for repeatable workload bytes, not cryptography.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint64_t internal_payload_rng(uint64_t* state)
{
  uint64_t value = *state;
  value ^= value << (uint8_t)k_cbs_payload_shift_a;
  value ^= value >> (uint8_t)k_cbs_payload_shift_b;
  value ^= value << (uint8_t)k_cbs_payload_shift_c;
  *state = value;
  return value;
}

/**
 * @brief Emit one byte from the deterministic pseudo-text state machine.
 * @details Selects words through ::internal_payload_rng and inserts bounded
 *          spaces, periods, and newlines according to the sentence phase.
 * @param[in,out] state Generator cursor to advance by one output byte.
 * @return Next deterministic payload byte.
 * @retval other ASCII word or separator byte.
 * @pre @p state is non-NULL and initialized from the payload seed.
 * @pre `state->phase` is a valid ::cbs_payload_phase_t value.
 * @post The returned byte matches the advanced state position.
 * @post Exactly one logical payload byte is consumed.
 * @note Thread-safe for distinct generator states.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_payload_next(cbs_payload_state_t* state)
{
  if (state->phase == (uint8_t)k_cbs_phase_dot) {
    state->phase = (uint8_t)k_cbs_phase_newline;
    return (uint8_t)'.';
  }
  if (state->phase == (uint8_t)k_cbs_phase_newline) {
    state->phase      = (uint8_t)k_cbs_phase_word;
    state->word_index = (uint32_t)k_cbs_no_word;
    return (uint8_t)'\n';
  }
  if (state->phase == (uint8_t)k_cbs_phase_space) {
    state->phase      = (uint8_t)k_cbs_phase_word;
    state->word_index = (uint32_t)k_cbs_no_word;
    return (uint8_t)' ';
  }
  if (state->word_index == (uint32_t)k_cbs_no_word) {
    const uint32_t count = (uint32_t)(sizeof(s_payload_words) / sizeof(s_payload_words[0]));
    state->word_index    = (uint32_t)(internal_payload_rng(&state->rng) % count);
    state->word_offset   = 0U;
  }
  const char* word = s_payload_words[state->word_index];
  if (word[state->word_offset] != '\0') {
    const uint8_t result = (uint8_t)word[state->word_offset];
    state->word_offset++;
    return result;
  }
  state->sentence_words++;
  if (state->sentence_words == (uint32_t)k_cbs_words_per_dot) {
    state->sentence_words = 0U;
    state->phase          = (uint8_t)k_cbs_phase_newline;
    return (uint8_t)'.';
  }
  state->phase      = (uint8_t)k_cbs_phase_word;
  state->word_index = (uint32_t)k_cbs_no_word;
  return (uint8_t)' ';
}

RA8_PRIV
size_t priv_payload_workspace_required(void)
{
  const size_t count = ((size_t)k_cbs_blob_bytes / k_cbs_payload_checkpoint_bytes) + 1U;
  return count * sizeof(cbs_payload_state_t);
}

RA8_PRIV
int priv_payload_init(cbs_payload_t* payload, void* workspace, size_t capacity)
{
  const size_t required = priv_payload_workspace_required();
  if ((payload == nullptr) || (workspace == nullptr) ||
      (((uintptr_t)workspace % alignof(cbs_payload_state_t)) != 0U) || (capacity < required)) {
    return 1;
  }
  cbs_payload_state_t* checkpoints = (cbs_payload_state_t*)workspace;
  cbs_payload_state_t  state       = {.rng        = (uint64_t)k_cbs_payload_seed,
                                      .word_index = (uint32_t)k_cbs_no_word};
  const uint32_t       count       = (uint32_t)(required / sizeof(cbs_payload_state_t));
  for (uint32_t index = 0U; index < count; ++index) {
    checkpoints[index] = state;
    if ((index + 1U) < count) {
      for (uint32_t i = 0U; i < (uint32_t)k_cbs_payload_checkpoint_bytes; ++i) {
        (void)internal_payload_next(&state);
      }
    }
  }
  *payload = (cbs_payload_t){.checkpoints      = checkpoints,
                             .checkpoint_count = count,
                             .cursor_state     = checkpoints[0]};
  return 0;
}

RA8_PRIV
ra8_err_t priv_payload_read(void* ctx, uint64_t offset, uint8_t* buffer, uint32_t length)
{
  cbs_payload_t* payload = (cbs_payload_t*)ctx;
  if ((payload == nullptr) || (buffer == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((offset + length) > (uint64_t)k_cbs_blob_bytes) {
    return k_ra8_err_out_of_range;
  }
  cbs_payload_state_t state = payload->cursor_state;
  if (offset != payload->cursor_offset) {
    const uint32_t checkpoint = (uint32_t)(offset / k_cbs_payload_checkpoint_bytes);
    if (checkpoint >= payload->checkpoint_count) {
      return k_ra8_err_out_of_range;
    }
    state               = ((const cbs_payload_state_t*)payload->checkpoints)[checkpoint];
    const uint32_t skip = (uint32_t)(offset % k_cbs_payload_checkpoint_bytes);
    for (uint32_t i = 0U; i < skip; ++i) {
      (void)internal_payload_next(&state);
    }
  }
  for (uint32_t i = 0U; i < length; ++i) {
    buffer[i] = internal_payload_next(&state);
  }
  payload->cursor_offset = offset + length;
  payload->cursor_state  = state;
  return k_ra8_ok;
}
