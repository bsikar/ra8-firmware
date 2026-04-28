/**
 * @file ra_bscan.c
 * @brief JTAG / IEEE-1149.1 Boundary Scan TAP HAL implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Tracks the firmware-side view of the RA8D2 boundary-scan Test
 * Access Port. The four TAP registers (JTIR, JTIDR, JTBPR, JTBSR)
 * described in HUM Ch 50.2 (p 3258-3259) are not memory-mapped --
 * they live behind the JTAG pin interface and are accessible only to
 * an external manufacturing-test fixture. This driver therefore
 * carries no register accesses; it only maintains a small cached
 * status object that other firmware modules can query.
 *
 * Citation strategy: every public API includes at least one HUM Ch 50
 * citation in a comment to anchor the design intent in the
 * spec, even though no register write actually happens.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_bscan.h"

#include <stdint.h>

#include "ra8d2_bscan_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/**
 * @var s_tag
 * @brief Log tag used by every ``ra_log_*`` call in this driver.
 */
static const char* s_tag = "BSCAN";

/**
 * @enum ra_bscan_clear_arg_t
 * @brief Allowed values for the ``mask`` argument of
 *        ``ra_bscan_clear_status``.
 *
 * @details
 * Sibling drivers (ACMPHS, GLCDC, ...) accept a 32-bit bitmask of
 * status flags to clear. This driver has no hardware flags so the
 * only legal value is zero. The enum makes that contract explicit.
 */
typedef enum : uint32_t {
  k_ra_bscan_clear_mask_none = 0U, /**< No-op mask -- the only legal value. */
} ra_bscan_clear_arg_t;

/**
 * @struct ra_bscan_state_t
 * @brief Firmware-side TAP bookkeeping object.
 */
typedef struct {
  bool             initialised;      /**< True between init and deinit.        */
  ra_bscan_instr_t last_instruction; /**< Last opcode the fixture reported.    */
  uint32_t         expected_idcode;  /**< JTIDR value the fixture should see.  */
} ra_bscan_state_t;

/**
 * @var s_bscan_state
 * @brief Singleton bookkeeping state. Updated only by this file.
 *
 * @note Not thread-safe; callers must serialise access from a single
 *       init / shutdown context.
 */
static ra_bscan_state_t s_bscan_state = {
  .initialised      = false,
  .last_instruction = k_ra_bscan_instr_bypass,
  .expected_idcode  = 0U,
};

/**
 * @brief Validate that an instruction code is one of the named opcodes.
 *
 * @param[in] instr Candidate opcode.
 * @return true if ``instr`` matches one of the values in
 *         HUM Ch 50.2.1 Table (p 3258); false for any reserved code.
 */
static bool internal_is_known_instruction(ra_bscan_instr_t instr)
{
  /* HUM Ch 50.2.1 "JTIR : Instruction Register", p 3258 */
  switch (instr) {
    case k_ra_bscan_instr_extest:
    case k_ra_bscan_instr_sample_preload:
    case k_ra_bscan_instr_idcode:
    case k_ra_bscan_instr_clamp:
    case k_ra_bscan_instr_highz:
    case k_ra_bscan_instr_bypass:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] ra_err_t ra_bscan_init(void)
{
  /* HUM Ch 50.2 "Register Descriptions", Table 50.3 p 3258 -- JTIDR
   * is hardwired to 0x085D_A447 and the JTIR power-on value is 0xE
   * (BYPASS path selected by the TAP controller on reset). */
  s_bscan_state.initialised      = true;
  s_bscan_state.last_instruction = k_ra_bscan_instr_bypass;
  s_bscan_state.expected_idcode  = (uint32_t)k_ra_bscan_jtidr_reset;
  ra_log_info(s_tag, "bscan init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bscan_deinit(void)
{
  /* HUM Ch 50.3 "Operation", p 3259 -- TAP returns to its reset
   * state when the external fixture holds TMS=1 for >=5 TCK clocks.
   * From a CPU-side bookkeeping viewpoint we just drop our cached
   * fields back to the power-on equivalents. */
  s_bscan_state.initialised      = false;
  s_bscan_state.last_instruction = k_ra_bscan_instr_bypass;
  s_bscan_state.expected_idcode  = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bscan_get_idcode(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!s_bscan_state.initialised) {
    return k_ra_err_not_initialized;
  }
  /* HUM Ch 50.2.2 "JTIDR : ID Code Register", p 3258-3259 -- DID[31:0]
   * is fixed at 0x085D_A447 by Renesas. The constant is the source of
   * truth; no hardware fetch is possible. */
  *out = s_bscan_state.expected_idcode;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bscan_get_status(ra_bscan_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 50.2.3 "JTBPR : Bypass Register", p 3259 explicitly states
   * the TAP registers cannot be read from the CPU -- so this is a
   * pure firmware-side snapshot. */
  out->initialised      = s_bscan_state.initialised;
  out->last_instruction = s_bscan_state.last_instruction;
  out->expected_idcode  = s_bscan_state.expected_idcode;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bscan_clear_status(uint32_t mask)
{
  if (mask != (uint32_t)k_ra_bscan_clear_mask_none) {
    return k_ra_err_invalid_arg;
  }
  if (!s_bscan_state.initialised) {
    return k_ra_err_not_initialized;
  }
  /* HUM Ch 50.2.1 "JTIR : Instruction Register", p 3258 -- the TAP
   * reset value 0xE selects the BYPASS path, so "cleared" maps to
   * BYPASS in our bookkeeping. */
  s_bscan_state.last_instruction = k_ra_bscan_instr_bypass;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_bscan_set_instruction(ra_bscan_instr_t instr)
{
  if (!s_bscan_state.initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_is_known_instruction(instr)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 50.2.1 "JTIR : Instruction Register", p 3258 -- valid
   * opcodes are EXTEST / SAMPLE_PRELOAD / IDCODE / CLAMP / HIGHZ /
   * BYPASS. All other 4-bit codes are reserved and rejected above. */
  s_bscan_state.last_instruction = instr;
  ra_log_info_val(s_tag, "set instruction", (uint32_t)instr);
  return k_ra_ok;
}
