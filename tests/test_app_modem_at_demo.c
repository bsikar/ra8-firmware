/**
 * @file test_app_modem_at_demo.c
 * @brief Integration test: modem_at_demo decision logic + real ra8_modem_at run
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_pending/modem_at_demo/main.c. The AT
 * command/response transport is owned + covered by ra8_modem_at's own unit
 * tests (test_ra8_modem_at.c); this test pins down the demo's app-level logic:
 *
 *  - The four compound decisions the app makes, each with full MC/DC:
 *    registration OK (``+CREG`` status home|roaming), signal usable (known AND
 *    strong), per-step expected outcome (the error-probe step inverts the pass
 *    condition), and the overall five-condition verdict.
 *  - The two reply-parsing helpers (first-uint-after-colon for CSQ / CGATT,
 *    last-uint for +CREG) that feed those decisions.
 *  - An end-to-end run of the REAL ra8_modem_at driver over an in-test AT
 *    responder that replays the exact bytes the ra8_emulator SCI7 modem model
 *    answers (board_periph_modem.c), so the whole demo flow -- sync, SIM,
 *    signal, registration + URC, PS attach, +CME ERROR path -- is exercised
 *    against the production driver and the verdict is asserted PASS.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_modem_at.h"
#include "unity_minimal.h"

/**
 * @enum app_modem_at_demo_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_modem_val_multi_digit = 123U, /**< A multi-digit value, exercising the accumulate loop. */
  k_modem_val_single_digit =
    7U, /**< A single-digit response value, the shortest the parser can see. */
  k_modem_line_cap =
    128, /**< Capacity of the end-to-end response line buffer, over the longest fixture response. */
  k_modem_capture_cap = 64, /**< Capacity of the captured-command buffer. */
} app_modem_at_demo_fixture_t;

/* ------------------------------------------------------------------------- */
/* Mirrors of the demo's app-local constants + pure decision logic. */
/* ------------------------------------------------------------------------- */

/** @brief Mirror of the demo's CSQ sentinels. */
typedef enum : uint8_t {
  k_t_csq_min_bars = 5U,  /**< Minimum usable RSSI.      */
  k_t_csq_max      = 31U, /**< Highest valid RSSI index. */
  k_t_csq_unknown  = 99U, /**< CSQ "unknown".            */
} t_csq_t;

/** @brief Mirror of the demo's +CREG status codes. */
typedef enum : uint8_t {
  k_t_creg_not_registered = 0U, /**< T creg not registered. */
  k_t_creg_home           = 1U, /**< T creg home.           */
  k_t_creg_searching      = 2U, /**< T creg searching.      */
  k_t_creg_denied         = 3U, /**< T creg denied.         */
  k_t_creg_unknown        = 4U, /**< T creg unknown.        */
  k_t_creg_roaming        = 5U, /**< T creg roaming.        */
} t_creg_t;

/** @brief Mirror of the demo's CGATT flag values. */
typedef enum : uint8_t {
  k_t_cgatt_detached = 0U, /**< T cgatt detached. */
  k_t_cgatt_attached = 1U, /**< T cgatt attached. */
} t_cgatt_t;

/** @brief Static scan bound shared by the parse mirrors (NASA P10 Rule 2). */
typedef enum : uint32_t {
  k_t_scan_max = 64U, /**< T scan maximum. */
  k_t_dec_base = 10U, /**< T dec base.     */
} t_scan_t;

/** @brief Mirror of ``modem_creg_registered``. */
static bool modem_creg_registered(uint32_t stat)
{
  return (stat == (uint32_t)k_t_creg_home) || (stat == (uint32_t)k_t_creg_roaming);
}

/** @brief Mirror of ``modem_signal_valid``. */
static bool modem_signal_valid(uint32_t rssi)
{
  return (rssi != (uint32_t)k_t_csq_unknown) && (rssi >= (uint32_t)k_t_csq_min_bars);
}

/** @brief Mirror of ``modem_step_passed``. */
static bool modem_step_passed(bool is_cme_probe, ra8_err_t err)
{
  return (is_cme_probe && (err == k_ra8_err_hw_error)) || ((!is_cme_probe) && (err == k_ra8_ok));
}

/** @brief Mirror of ``modem_run_ok``. */
static bool
modem_run_ok(bool sim_ready, bool signal_ok, bool registered, bool attached, bool cme_ok)
{
  return sim_ready && signal_ok && registered && attached && cme_ok;
}

/** @brief Mirror of ``modem_parse_first_uint``. */
static bool modem_parse_first_uint(const char* line, uint32_t* out)
{
  if ((line == nullptr) || (out == nullptr)) {
    return false;
  }
  uint32_t i = 0U;
  while ((i < (uint32_t)k_t_scan_max) && (line[i] != '\0') && (line[i] != ':')) {
    i++;
  }
  if ((i >= (uint32_t)k_t_scan_max) || (line[i] != ':')) {
    return false;
  }
  i++;
  while ((i < (uint32_t)k_t_scan_max) && (line[i] == ' ')) {
    i++;
  }
  if ((i >= (uint32_t)k_t_scan_max) || (line[i] < '0') || (line[i] > '9')) {
    return false;
  }
  uint32_t v = 0U;
  while ((i < (uint32_t)k_t_scan_max) && (line[i] >= '0') && (line[i] <= '9')) {
    v = (v * (uint32_t)k_t_dec_base) + (uint32_t)(line[i] - '0');
    i++;
  }
  *out = v;
  return true;
}

/** @brief Mirror of ``modem_parse_last_uint``. */
static bool modem_parse_last_uint(const char* line, uint32_t* out)
{
  if ((line == nullptr) || (out == nullptr)) {
    return false;
  }
  bool     found = false;
  uint32_t v     = 0U;
  uint32_t i     = 0U;
  while ((i < (uint32_t)k_t_scan_max) && (line[i] != '\0')) {
    if ((line[i] >= '0') && (line[i] <= '9')) {
      v = 0U;
      while ((i < (uint32_t)k_t_scan_max) && (line[i] >= '0') && (line[i] <= '9')) {
        v = (v * (uint32_t)k_t_dec_base) + (uint32_t)(line[i] - '0');
        i++;
      }
      found = true;
    } else {
      i++;
    }
  }
  if (found) {
    *out = v;
  }
  return found;
}

/** @brief Mirror of ``modem_line_contains``. */
static bool modem_line_contains(const char* line, const char* needle)
{
  if ((line == nullptr) || (needle == nullptr)) {
    return false;
  }
  for (uint32_t i = 0U; (i < (uint32_t)k_t_scan_max) && (line[i] != '\0'); i++) {
    uint32_t j = 0U;
    while ((j < (uint32_t)k_t_scan_max) && (needle[j] != '\0') && (line[i + j] == needle[j])) {
      j++;
    }
    if (needle[j] == '\0') {
      return true;
    }
  }
  return false;
}

/* ------------------------------------------------------------------------- */
/* MC/DC tests for the four compound decisions. */
/* ------------------------------------------------------------------------- */

/**
 * @test modem_creg_registered MC/DC
 *
 * @par MC/DC:
 * Decision: ``(stat == 1) || (stat == 5)`` (2 conditions, OR). N+1 = 3 vectors:
 * - Vector 1: stat=0  -> false (control: both conditions false).
 * - Vector 2: stat=1  -> true  (varies cond1; cond2 false in both 0 and 1).
 * - Vector 3: stat=5  -> true  (varies cond2; cond1 false in both 0 and 5).
 * Vectors 1+2 prove ``stat==1`` independently flips the outcome; 1+3 prove the
 * same for ``stat==5``.
 */
static void test_creg_registered_mcdc(void)
{
  TEST_BEGIN("modem_at_demo: creg registered MC/DC");
  TEST_ASSERT(!modem_creg_registered((uint32_t)k_t_creg_not_registered)); /* 0 -> false */
  TEST_ASSERT(modem_creg_registered((uint32_t)k_t_creg_home));            /* 1 -> true  */
  TEST_ASSERT(modem_creg_registered((uint32_t)k_t_creg_roaming));         /* 5 -> true  */
  /* Extra non-registered codes stay false. */
  TEST_ASSERT(!modem_creg_registered((uint32_t)k_t_creg_searching));
  TEST_ASSERT(!modem_creg_registered((uint32_t)k_t_creg_denied));
  TEST_ASSERT(!modem_creg_registered((uint32_t)k_t_creg_unknown));
  TEST_END("modem_at_demo: creg registered MC/DC");
}

/**
 * @test modem_signal_valid MC/DC
 *
 * @par MC/DC:
 * Decision: ``(rssi != 99) && (rssi >= 5)`` (2 conditions, AND). The conditions
 * are independent -- 99 (unknown) is above the strength floor, so either can
 * fail alone. N+1 = 3 vectors:
 * - Vector 1: rssi=17 -> true  (control: both conditions true).
 * - Vector 2: rssi=99 -> false (varies cond1: unknown; cond2 held true: 99>=5).
 * - Vector 3: rssi=2  -> false (varies cond2: below floor; cond1 held true).
 * Vectors 1+2 prove ``rssi!=99`` independently affects the outcome; 1+3 prove
 * the same for ``rssi>=5``.
 */
static void test_signal_valid_mcdc(void)
{
  TEST_BEGIN("modem_at_demo: signal valid MC/DC");
  TEST_ASSERT(modem_signal_valid(17U));                        /* known + strong -> T */
  TEST_ASSERT(!modem_signal_valid((uint32_t)k_t_csq_unknown)); /* unknown (99)   -> F */
  TEST_ASSERT(!modem_signal_valid(2U));                        /* weak (< 5)     -> F */
  /* Boundary: exactly the usable floor is accepted. */
  TEST_ASSERT(modem_signal_valid((uint32_t)k_t_csq_min_bars));
  TEST_END("modem_at_demo: signal valid MC/DC");
}

/**
 * @test modem_step_passed MC/DC
 *
 * @par MC/DC:
 * Decision: ``(P && (err==hw_error)) || (!P && (err==ok))`` where P is
 * ``is_cme_probe`` and err is the step's return code. N+1 = 4 vectors over the
 * (P, err-class) inputs:
 * - Vector 1: P=1, err=hw_error -> true  (probe sees the expected CME error).
 * - Vector 2: P=1, err=ok       -> false (varies err with P=1: a probe that
 *                                          did NOT error is a failure).
 * - Vector 3: P=0, err=ok       -> true  (varies P vs vector 2 with err=ok).
 * - Vector 4: P=0, err=hw_error -> false (varies err with P=0).
 * 1+2 isolate err's effect when P=1; 3+4 isolate err's effect when P=0; 2+3
 * isolate P's effect when err=ok.
 */
static void test_step_passed_mcdc(void)
{
  TEST_BEGIN("modem_at_demo: step passed MC/DC");
  TEST_ASSERT(modem_step_passed(true, k_ra8_err_hw_error));   /* probe + CME error -> T */
  TEST_ASSERT(!modem_step_passed(true, k_ra8_ok));            /* probe + OK        -> F */
  TEST_ASSERT(modem_step_passed(false, k_ra8_ok));            /* normal + OK       -> T */
  TEST_ASSERT(!modem_step_passed(false, k_ra8_err_hw_error)); /* normal + error   -> F  */
  /* A timeout on a normal step is also a failure. */
  TEST_ASSERT(!modem_step_passed(false, k_ra8_err_hw_timeout));
  TEST_END("modem_at_demo: step passed MC/DC");
}

/**
 * @test modem_run_ok MC/DC
 *
 * @par MC/DC:
 * Decision: ``sim_ready && signal_ok && registered && attached && cme_ok``
 * (5 conditions, AND). N+1 = 6 vectors: the all-true control plus one vector
 * per condition flipped to false (each independently drives the outcome false
 * because the other four are held true).
 */
static void test_run_ok_mcdc(void)
{
  TEST_BEGIN("modem_at_demo: run verdict MC/DC");
  TEST_ASSERT(modem_run_ok(true, true, true, true, true));   /* control         */
  TEST_ASSERT(!modem_run_ok(false, true, true, true, true)); /* vary sim_ready  */
  TEST_ASSERT(!modem_run_ok(true, false, true, true, true)); /* vary signal_ok  */
  TEST_ASSERT(!modem_run_ok(true, true, false, true, true)); /* vary registered */
  TEST_ASSERT(!modem_run_ok(true, true, true, false, true)); /* vary attached   */
  TEST_ASSERT(!modem_run_ok(true, true, true, true, false)); /* vary cme_ok     */
  TEST_END("modem_at_demo: run verdict MC/DC");
}

/* ------------------------------------------------------------------------- */
/* Parse-helper correctness (feeds the decisions above). */
/* ------------------------------------------------------------------------- */

/**
 * @test modem_parse_first_uint
 *
 * @par MC/DC:
 * The inner accept guard ``(line[i] >= '0') && (line[i] <= '9')`` (2 conditions,
 * AND) is driven by: a digit (both true, parsed), a pre-digit non-digit like
 * ':' handling, and the terminating non-digit that ends the run. The
 * colon/no-colon and digit/no-digit reject paths are covered by the negative
 * cases below.
 */
static void test_parse_first_uint(void)
{
  TEST_BEGIN("modem_at_demo: parse first uint");
  uint32_t v = k_modem_val_multi_digit;
  TEST_ASSERT(modem_parse_first_uint("+CSQ: 17,99", &v));
  TEST_ASSERT_EQ(17U, v);
  TEST_ASSERT(modem_parse_first_uint("+CGATT: 1", &v));
  TEST_ASSERT_EQ(1U, v);
  TEST_ASSERT(modem_parse_first_uint("+CGATT:0", &v)); /* no space after colon */
  TEST_ASSERT_EQ(0U, v);
  /* Reject paths: no colon, colon-but-no-digit, NULL args. */
  TEST_ASSERT(!modem_parse_first_uint("OK", &v));
  TEST_ASSERT(!modem_parse_first_uint("+CPIN: READY", &v));
  TEST_ASSERT(!modem_parse_first_uint(nullptr, &v));
  TEST_ASSERT(!modem_parse_first_uint("+CSQ: 1", nullptr));
  TEST_END("modem_at_demo: parse first uint");
}

/**
 * @test modem_parse_last_uint
 *
 * @par MC/DC:
 * Covers the last-digit-run scan on both +CREG forms: the URC ``+CREG: <stat>``
 * (single field) and the solicited ``+CREG: <n>,<stat>`` (two fields) both
 * yield ``stat`` as the last run. The no-digit reject and NULL guards exercise
 * the ``found == false`` path.
 */
static void test_parse_last_uint(void)
{
  TEST_BEGIN("modem_at_demo: parse last uint");
  uint32_t v = k_modem_val_single_digit;
  TEST_ASSERT(modem_parse_last_uint("+CREG: 1", &v)); /* URC form */
  TEST_ASSERT_EQ(1U, v);
  TEST_ASSERT(modem_parse_last_uint("+CREG: 1,5", &v)); /* n,stat */
  TEST_ASSERT_EQ(5U, v);
  TEST_ASSERT(modem_parse_last_uint("+CREG: 0,1", &v)); /* home */
  TEST_ASSERT_EQ(1U, v);
  /* Reject: no digit anywhere, and NULL guards. */
  TEST_ASSERT(!modem_parse_last_uint("+CREG: ", &v));
  TEST_ASSERT(!modem_parse_last_uint(nullptr, &v));
  TEST_ASSERT(!modem_parse_last_uint("+CREG: 1", nullptr));
  TEST_END("modem_at_demo: parse last uint");
}

/**
 * @test modem_line_contains
 *
 * @par MC/DC:
 * The inner match guard ``(needle[j] != '\0') && (line[i+j] == needle[j])``
 * (2 conditions, AND) is driven by a full match (needle exhausted -> true), a
 * mismatch mid-needle (second condition false -> advance), and a needle longer
 * than the tail (line NUL ends the compare -> no match).
 */
static void test_line_contains(void)
{
  TEST_BEGIN("modem_at_demo: line contains");
  TEST_ASSERT(modem_line_contains("+CPIN: READY", "READY"));
  TEST_ASSERT(modem_line_contains("READY", "READY")); /* match at index 0 */
  TEST_ASSERT(!modem_line_contains("+CPIN: SIM PIN", "READY"));
  TEST_ASSERT(!modem_line_contains("READ", "READY")); /* needle overruns tail */
  TEST_ASSERT(!modem_line_contains(nullptr, "READY"));
  TEST_ASSERT(!modem_line_contains("READY", nullptr));
  TEST_END("modem_at_demo: line contains");
}

/* ------------------------------------------------------------------------- */
/* End-to-end: the real ra8_modem_at driver over an in-test AT responder */
/* that replays the exact ra8_emulator SCI7 modem model bytes. */
/* ------------------------------------------------------------------------- */

/** @brief FIFO sizing for the in-test transport. */
typedef enum : uint16_t {
  k_t_fifo_cap = 1024U, /**< T FIFO cap. */
  k_t_cmd_cap  = 64U,   /**< T cmd cap.  */
} t_fifo_t;

/** @brief One byte FIFO (modem<->MCU). */
typedef struct {
  uint8_t  buf[k_t_fifo_cap]; /**< Buffer. */
  uint16_t head;              /**< Head.   */
  uint16_t tail;              /**< Tail.   */
} t_fifo_state_t;

/** @brief In-test transport: mirrors board_periph_modem.c line semantics. */
typedef struct {
  t_fifo_state_t modem_to_mcu;     /**< Bytes the modem sends the MCU (rx path). */
  char           cmd[k_t_cmd_cap]; /**< Cmd.                                     */
  uint16_t       cmd_len;          /**< Cmd length.                              */
} t_modem_io_t;

static t_modem_io_t s_io;

/** @brief The AT script (identical to board_periph_modem.c's k_modem_script). */
typedef struct {
  const char* cmd;  /**< Cmd.  */
  const char* resp; /**< Resp. */
} t_reply_t;

static const t_reply_t k_t_script[] = {
  {"AT", "\r\nOK\r\n"},
  {"ATE0", "\r\nOK\r\n"},
  {"AT+CMEE=1", "\r\nOK\r\n"},
  {"AT+CPIN?", "\r\n+CPIN: READY\r\n\r\nOK\r\n"},
  {"AT+CSQ", "\r\n+CSQ: 17,99\r\n\r\nOK\r\n"},
  {"AT+CREG=1", "\r\nOK\r\n\r\n+CREG: 1\r\n"},
  {"AT+CREG?", "\r\n+CREG: 1,1\r\n\r\nOK\r\n"},
  {"AT+CGATT?", "\r\n+CGATT: 1\r\n\r\nOK\r\n"},
};
static const char k_t_cme_error[] = "\r\n+CME ERROR: 4\r\n";

static void fifo_push(t_fifo_state_t* f, uint8_t b)
{
  if (f->tail < (uint16_t)k_t_fifo_cap) {
    f->buf[f->tail] = b;
    f->tail++;
  }
}

static void fifo_push_str(t_fifo_state_t* f, const char* s)
{
  for (uint32_t i = 0U; s[i] != '\0'; i++) {
    fifo_push(f, (uint8_t)s[i]);
  }
}

/** @brief Answer the accumulated command by staging its response bytes. */
static void modem_answer(void)
{
  s_io.cmd[s_io.cmd_len] = '\0';
  const uint32_t n       = (uint32_t)(sizeof(k_t_script) / sizeof(k_t_script[0]));
  for (uint32_t i = 0U; i < n; i++) {
    if (strcmp(s_io.cmd, k_t_script[i].cmd) == 0) {
      fifo_push_str(&s_io.modem_to_mcu, k_t_script[i].resp);
      s_io.cmd_len = 0U;
      return;
    }
  }
  fifo_push_str(&s_io.modem_to_mcu, k_t_cme_error);
  s_io.cmd_len = 0U;
}

/* cppcheck-suppress constParameterCallback ; must match the io.tx_byte typedef. */
static ra8_err_t t_tx(void* ctx, uint8_t byte)
{
  (void)ctx;
  if (byte == (uint8_t)'\r') {
    modem_answer();
  } else if (byte != (uint8_t)'\n') {
    if (s_io.cmd_len < (uint16_t)(k_t_cmd_cap - 1U)) {
      s_io.cmd[s_io.cmd_len] = (char)byte;
      s_io.cmd_len++;
    }
  }
  return k_ra8_ok;
}

/* cppcheck-suppress constParameterCallback ; must match the io.rx_byte typedef. */
static ra8_err_t t_rx(void* ctx, uint8_t* out)
{
  (void)ctx;
  t_fifo_state_t* f = &s_io.modem_to_mcu;
  if (f->head >= f->tail) {
    return k_ra8_err_no_data;
  }
  *out = f->buf[f->head];
  f->head++;
  return k_ra8_ok;
}

/* cppcheck-suppress constParameterCallback ; must match the io.now_ms typedef. */
static uint32_t t_now(void* ctx)
{
  (void)ctx;
  return 0U; /* responses are always pre-staged, so time never advances */
}

/** @brief Advancing fake clock for the no-modem timeout test. */
static uint32_t s_fake_ms;

/* cppcheck-suppress constParameterCallback ; must match the io.now_ms typedef. */
static uint32_t t_now_advancing(void* ctx)
{
  (void)ctx;
  s_fake_ms++;
  return s_fake_ms;
}

/* cppcheck-suppress constParameterCallback ; must match the io.tx_byte typedef. */
static ra8_err_t t_tx_silent(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
  return k_ra8_ok; /* a wire with no modem: bytes go out, nothing comes back */
}

/* cppcheck-suppress constParameterCallback ; must match the io.rx_byte typedef. */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t t_rx_empty(void* ctx, uint8_t* out)
{
  (void)ctx;
  (void)out;
  return k_ra8_err_no_data; /* no modem -> RX FIFO is always empty */
}

/** @brief Latched +CREG status from the URC handler (mirrors the app). */
static uint8_t s_e2e_creg_stat;
static bool    s_e2e_creg_seen;

static void t_on_creg(const char* line, void* ctx)
{
  (void)ctx;
  uint32_t stat = 0U;
  if (modem_parse_last_uint(line, &stat)) {
    s_e2e_creg_stat = (uint8_t)stat;
    s_e2e_creg_seen = true;
  }
}

static uint8_t s_e2e_line[k_modem_line_cap];

/**
 * @test modem_at_demo end-to-end verdict is PASS
 *
 * @par MC/DC:
 * No new compound decision -- this drives the real ra8_modem_at over the exact
 * ra8_emulator SCI7 responder bytes and asserts the demo's five-condition verdict
 * (::modem_run_ok) evaluates true, confirming the mirrored decisions above
 * agree with the production driver on realistic modem traffic.
 */
static void test_end_to_end_pass(void)
{
  TEST_BEGIN("modem_at_demo: end-to-end PASS");
  s_io            = (t_modem_io_t){};
  s_e2e_creg_stat = 0U;
  s_e2e_creg_seen = false;

  const ra8_modem_at_cfg_t cfg = {
    .io                 = {.tx_byte = t_tx, .rx_byte = t_rx, .now_ms = t_now, .ctx = nullptr},
    .line_buf           = s_e2e_line,
    .line_buf_len       = (uint16_t)sizeof s_e2e_line,
    .default_timeout_ms = 2000U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_register_unsolicited_handler("+CREG:", t_on_creg, nullptr));

  /* sync phase */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT", nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("ATE0", nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT+CMEE=1", nullptr, 0U));

  /* SIM phase */
  char cap[k_modem_capture_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd_capture("AT+CPIN?", cap, sizeof cap, 0U));
  const bool sim_ready = modem_line_contains(cap, "READY");
  TEST_ASSERT(sim_ready);

  /* signal phase */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd_capture("AT+CSQ", cap, sizeof cap, 0U));
  uint32_t rssi = (uint32_t)k_t_csq_unknown;
  TEST_ASSERT(modem_parse_first_uint(cap, &rssi));
  TEST_ASSERT_EQ(17U, rssi);
  const bool signal_ok = modem_signal_valid(rssi);
  TEST_ASSERT(signal_ok);

  /* registration phase: enable URCs, drain the +CREG URC, then query */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT+CREG=1", nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_poll()); /* dispatch the +CREG URC */
  TEST_ASSERT(s_e2e_creg_seen);
  TEST_ASSERT_EQ(k_t_creg_home, s_e2e_creg_stat);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd("AT+CREG?", nullptr, 0U));
  const bool registered = s_e2e_creg_seen && modem_creg_registered((uint32_t)s_e2e_creg_stat);
  TEST_ASSERT(registered);

  /* attach phase */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_send_cmd_capture("AT+CGATT?", cap, sizeof cap, 0U));
  uint32_t flag = (uint32_t)k_t_cgatt_detached;
  TEST_ASSERT(modem_parse_first_uint(cap, &flag));
  const bool attached = (flag == (uint32_t)k_t_cgatt_attached);
  TEST_ASSERT(attached);

  /* error path: unsupported command must surface hw_error */
  const ra8_err_t cme_err = ra8_modem_at_send_cmd("AT+NOSUCHCMD", nullptr, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, cme_err);
  const bool cme_ok = modem_step_passed(true, cme_err);
  TEST_ASSERT(cme_ok);

  /* overall verdict */
  TEST_ASSERT(modem_run_ok(sim_ready, signal_ok, registered, attached, cme_ok));
  TEST_END("modem_at_demo: end-to-end PASS");
}

/**
 * @test modem_at_demo end-to-end fails without a modem
 *
 * @par MC/DC:
 * No compound decision -- with no responder staging bytes, the first ``AT``
 * times out (rx returns no-data, ``now_ms`` never advances past the timeout so
 * the driver drains its bounded loop and reports ``k_ra8_err_hw_timeout``),
 * proving the demo would report FAIL rather than a false PASS (EIL == HIL: the
 * app only passes when a modem answers).
 */
static void test_end_to_end_no_modem(void)
{
  TEST_BEGIN("modem_at_demo: no-modem timeout");
  s_fake_ms = 0U;

  /* A silent wire (no modem answering) + an advancing clock so the driver's
   * bounded wait loop reaches its per-command timeout. */
  const ra8_modem_at_cfg_t cfg = {
    .io                 = {.tx_byte = t_tx_silent,
                           .rx_byte = t_rx_empty,
                           .now_ms  = t_now_advancing,
                           .ctx     = nullptr},
    .line_buf           = s_e2e_line,
    .line_buf_len       = (uint16_t)sizeof s_e2e_line,
    .default_timeout_ms = 1U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_modem_at_init(&cfg));
  const ra8_err_t err = ra8_modem_at_send_cmd("AT", nullptr, 1U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  /* The demo would therefore report FAIL, never a false PASS. */
  TEST_ASSERT(!modem_step_passed(false, err));
  TEST_END("modem_at_demo: no-modem timeout");
}

int main(void)
{
  test_creg_registered_mcdc();
  test_signal_valid_mcdc();
  test_step_passed_mcdc();
  test_run_ok_mcdc();
  test_parse_first_uint();
  test_parse_last_uint();
  test_line_contains();
  test_end_to_end_pass();
  test_end_to_end_no_modem();
  return 0;
}
