/**
 * @file examples/ek_ra8d2/hw_validated/hil/pdm_mic_demo/src/main.c
 * @brief PDM MEMS microphone capture + plausibility demo (SPH0690 / ch 2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Stand-alone EK-RA8D2 application that brings up the on-board SPH0690
 * PDM MEMS microphones and reports whether they produce plausible audio.
 *
 * Board wiring (EK-RA8D2 v1 UM Table 31 p 37): two SPH0690 mics (MIC1,
 * MIC2) on the PCB underside share one clock and one data line ->
 *   - PDMCLK2 = P812  (MIC pin 4, CLOCK)
 *   - PDMDAT2 = P502  (MIC pin 1, DATA)
 * MIC1 SELECT = GND (rise-edge data), MIC2 SELECT = +3.3V (fall-edge).
 * Both are on PDM-IF channel 2, so this demo captures MIC1 (rise edge).
 *
 * Signal path (all in hardware, HUM Ch 49): 4 MHz PDM_CLK2 (PDMIFCLK
 * = MOCO 8 MHz / 2) -> 4th-order sinc decimation (M=125) -> compensation
 * FIR -> high-pass (DC block) -> half-band decimation -> 16 kHz, 20-bit
 * signed PCM read out of PDDRRCH2. Filter coefficients are the RA8D2
 * reset-default SPH0690 set (HUM Ch 49.2 register reset values).
 *
 * Flow:
 *   1. Board clocks / ra8_mstp_init / ra8_time_init.
 *   2. Route P812/P502 to the PDM function; bring up the SCI8 console.
 *   3. Query board-owned SPH0690 policy and bind `ra8_audio_source_pdm`.
 *   4. The backend starts PDM-IF, settles, and discards the filter transient.
 *   5. Capture caller-owned fixed-size PCM frames through `ra8_audio`.
 *   6. Compute AC RMS / peak / span and a plausibility verdict, print
 *      the banner `pdm: rms=NNN peak=NNN active=Y` (scraped by hil.conf)
 *      and publish it in the J-Link-probable ::g_pdm_mic_result.
 *   7. Latch the PASS verdict and park in a low-power wait loop.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_audio.h"
#include "ra8_audio_source_pdm.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/* =============================================================================
 * App-wide tunables
 * =============================================================================
 */

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_pdm_demo_baud       = 115200U, /**< SCI8 console baud.                       */
  k_pdm_demo_decimal    = 10U,     /**< Radix for integer-to-ASCII.              */
  k_pdm_demo_settle_ms  = 300U,    /**< Mic wake-up + filter settle delay.       */
  k_pdm_demo_window     = 256U,    /**< PCM samples per analysis window.         */
  k_pdm_demo_discard    = 1024U,   /**< Transient samples discarded up front.    */
  k_pdm_demo_report_win = 8U,      /**< Windows swept for the verdict banner.    */
  k_pdm_demo_poll_max   = 500000U, /**< Bounded capture poll retries (dead-mic). */
} pdm_demo_const_t;

/** @brief Plausibility thresholds (20-bit signed full scale = +-524288). */
typedef enum : int32_t {
  k_pdm_demo_min_span  = 8,  /**< Min peak-to-peak to be non-degenerate. */
  k_pdm_demo_rms_floor = 16, /**< Min AC RMS to count as live audio.     */
} pdm_demo_threshold_t;

/** @brief Sizing for the decimal serialiser buffer ("-2147483648" + NUL). */
typedef enum : uint32_t {
  k_pdm_demo_int_str_cap = 12U, /**< Pdm demo int str cap. */
} pdm_demo_str_cap_t;

/** @brief Integer-sqrt bit-window constants (64-bit radicand). */
typedef enum : uint32_t {
  k_pdm_isqrt_steps = 32U, /**< 32 two-bit steps span a 64-bit radicand.  */
  k_pdm_isqrt_top2  = 62U, /**< Shift to the top 2 bits of a 64-bit word. */
} pdm_isqrt_const_t;

/* =============================================================================
 * Captured-window metrics + J-Link-probable result
 * =============================================================================
 */

/**
 * @struct pdm_metrics_t
 * @brief Statistics for one captured PCM window.
 */
typedef struct {
  int32_t  min;  /**< Minimum sample.                 */
  int32_t  max;  /**< Maximum sample.                 */
  int32_t  mean; /**< DC mean (near 0 after the HPF). */
  int32_t  peak; /**< Peak |sample - mean| (AC peak). */
  int32_t  rms;  /**< AC RMS about the mean.          */
  uint32_t n;    /**< Samples analysed.               */
} pdm_metrics_t;

/**
 * @struct pdm_mic_result_t
 * @brief Debugger-probable capture result.
 *
 * @details Latched every window so a J-Link read reflects the latest verdict.
 */
typedef struct {
  volatile uint32_t magic;   /**< 0x50444D31 ('PDM1') once a window is done. */
  volatile int32_t  rms;     /**< Latest window AC RMS.                      */
  volatile int32_t  peak;    /**< Latest window AC peak.                     */
  volatile int32_t  mean;    /**< Latest window DC mean.                     */
  volatile int32_t  span;    /**< Latest window max-min.                     */
  volatile int32_t  vary;    /**< RMS spread across the report sweep.        */
  volatile uint32_t samples; /**< Samples in the latest window.              */
  volatile uint32_t active;  /**< 1 = plausible live audio, 0 = degenerate.  */
  volatile uint32_t windows; /**< Windows captured so far.                   */
  /** @brief Latest frame capture-start timestamp. */
  volatile uint32_t timestamp_ms;
} pdm_mic_result_t;

/** @brief J-Link-probable capture result (read at ``&g_pdm_mic_result``). */
volatile pdm_mic_result_t g_pdm_mic_result;

/** @brief Static capture buffer (kept off the stack). */
static int32_t s_pdm_demo_buf[k_pdm_demo_window];

/** @brief Generic caller-owned audio source handle. */
static ra8_audio_source_t s_pdm_demo_source;

/** @brief Caller-owned PDM backend state. */
static ra8_audio_source_pdm_state_t s_pdm_demo_source_state;

/** @brief Result magic marker ('PDM1'). */
typedef enum : uint32_t {
  k_pdm_demo_magic = 0x50444D31U, /**< Pdm demo magic. */
} pdm_demo_magic_t;

/* =============================================================================
 * Banners
 * =============================================================================
 */

static const uint8_t k_pdm_demo_banner_init[]     = "pdm: init ch=2 clk=4mhz fs=16khz\r\n";
static const uint8_t k_pdm_demo_banner_fail[]     = "pdm: init FAIL\r\n";
static const uint8_t k_pdm_demo_banner_nodat[]    = "pdm: no data (FIFO empty) -- clock/mic?\r\n";
static const uint8_t k_pdm_demo_banner_pass[]     = "pdm: verdict=PASS active=Y\r\n";
static const uint8_t k_pdm_demo_banner_inactive[] = "pdm: verdict=FAIL active=N\r\n";

/* =============================================================================
 * Halt + tiny serialisers
 * =============================================================================
 */

/**
 * @brief Park forever after a fatal error.
 *
 * @details Waits in a low-power spin; only a debugger or reset wakes it.
 *
 * @return Never returns.
 *
 * @pre Called only after a fatal init failure.
 * @pre The console banner (if any) has been flushed.
 * @post The CPU is parked.
 * @post No further application code runs.
 *
 * @note Not callable from ISR context.
 * @since 0.1.0
 */
static void pdm_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Convert a signed 32-bit int into a decimal ASCII string.
 *
 * @details Avoids newlib printf. Buffer must hold ::k_pdm_demo_int_str_cap.
 *
 * @param[in]  value Integer to convert.
 * @param[out] out   Destination buffer (>= 12 bytes, non-NULL).
 *
 * @return Number of bytes written (excluding the NUL).
 * @retval uint32_t Decimal character count excluding the terminator.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``out`` has at least ::k_pdm_demo_int_str_cap bytes.
 * @post ``out`` holds a NUL-terminated decimal string.
 * @post The return value is in ``[1, 11]``.
 *
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
static uint32_t pdm_demo_i32_to_dec(int32_t value, uint8_t* out)
{
  uint8_t  tmp[k_pdm_demo_int_str_cap] = {};
  uint32_t n                           = 0U;
  bool     neg                         = value < 0;
  /* Work in int64 so INT32_MIN negates without overflow. */
  int64_t v = value;
  if (neg) {
    v = -v;
  }
  if (v == 0) {
    tmp[n++] = (uint8_t)'0';
  } else {
    while (v > 0 && n < (uint32_t)k_pdm_demo_int_str_cap) {
      tmp[n++] = (uint8_t)('0' + (uint8_t)(v % (int64_t)k_pdm_demo_decimal));
      v /= (int64_t)k_pdm_demo_decimal;
    }
  }
  if (neg && n < (uint32_t)k_pdm_demo_int_str_cap) {
    tmp[n++] = (uint8_t)'-';
  }
  for (uint32_t i = 0U; i < n; ++i) {
    out[i] = tmp[n - 1U - i];
  }
  if (n < (uint32_t)k_pdm_demo_int_str_cap) {
    out[n] = 0U;
  }
  return n;
}

/**
 * @brief Integer square root of a 64-bit value (floor).
 *
 * @details Bit-by-bit restoring algorithm; 32 fixed iterations, no FPU.
 *
 * @param[in] value Radicand.
 *
 * @return ``floor(sqrt(value))``.
 * @retval uint32_t Greatest integer whose square does not exceed `value`.
 *
 * @pre ``value`` is a mean-square accumulation.
 * @pre No FPU dependency is required.
 * @post The result squared does not exceed ``value``.
 * @post ``(result + 1)`` squared exceeds ``value``.
 *
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
static uint32_t pdm_demo_isqrt(uint64_t value)
{
  uint64_t rem  = 0U;
  uint64_t root = 0U;
  for (int32_t i = 0; i < (int32_t)k_pdm_isqrt_steps; ++i) {
    root <<= 1;
    rem = (rem << 2) | (value >> (uint32_t)k_pdm_isqrt_top2);
    value <<= 2;
    ++root;
    if (root <= rem) {
      rem -= root;
      ++root;
    } else {
      --root;
    }
  }
  return (uint32_t)(root >> 1);
}

/**
 * @brief Emit a byte run over the SCI8 console.
 * @details Forwards the exact caller span through the board console without formatting.
 *
 * @param[in] bytes Byte array (non-NULL).
 * @param[in] len   Byte count.
 *
 * @return Nothing.
 *
 * @pre The console was initialised.
 * @pre ``bytes`` is non-NULL.
 * @post ``len`` bytes were queued for transmit.
 * @post No application state changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void pdm_demo_tx(const uint8_t* bytes, uint32_t len)
{
  (void)ra8_board_uart_console_write(bytes, (size_t)len);
}

/**
 * @brief Emit ``"<label><value>"`` with a decimal integer.
 * @details Serializes the signed value into fixed stack storage after the label.
 *
 * @param[in] label     Label bytes (non-NULL).
 * @param[in] label_len Label length.
 * @param[in] value     Signed integer to print.
 *
 * @return Nothing.
 *
 * @pre The console was initialised.
 * @pre ``label`` is non-NULL.
 * @post The label and value were queued for transmit.
 * @post No application state changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void pdm_demo_emit_kv(const uint8_t* label, uint32_t label_len, int32_t value)
{
  uint8_t buf[k_pdm_demo_int_str_cap] = {};
  pdm_demo_tx(label, label_len);
  const uint32_t n = pdm_demo_i32_to_dec(value, buf);
  pdm_demo_tx(buf, n);
}

/* =============================================================================
 * Bring-up
 * =============================================================================
 */

/**
 * @brief Bring CGC + MSTP + SysTick up; halts on any failure.
 *
 * @details Runs on MOCO (~8 MHz), which also sources PDMIFCLK (8 MHz).
 *
 * @return Nothing.
 *
 * @pre Reset_Handler prepared .data/.bss.
 * @pre Single-threaded init context.
 * @post CGC, MSTP and the delay timer are live on success.
 * @post The CPU is parked on any failure.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void pdm_demo_clocks_or_halt(void)
{
  ra8_board_clock_rates_t clock_rates = {};
  if (ra8_board_clocks_init(&clock_rates) != k_ra8_ok) {
    pdm_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    pdm_demo_panic_halt();
  }
  if (ra8_time_init(clock_rates.cpuclk0_hz) != k_ra8_ok) {
    pdm_demo_panic_halt();
  }
}

/**
 * @brief Route P812/P502 to the PDM function and open the console.
 *
 * @details Both pins take PSEL ::k_ra8_psel_pdm; the console is SCI8.
 *
 * @return Nothing.
 *
 * @pre ::pdm_demo_clocks_or_halt already ran.
 * @pre Single-threaded init context.
 * @post PDMCLK2/PDMDAT2 are routed and the console is live on success.
 * @post The CPU is parked on any failure.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void pdm_demo_io_or_halt(void)
{
  if (ra8_board_pdm_mic_route() != k_ra8_ok) {
    pdm_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_pdm_demo_baud) != k_ra8_ok) {
    pdm_demo_panic_halt();
  }
  (void)ra8_board_led_init(k_ra8_board_led1);
}

/**
 * @brief Bind the on-board MIC1 through the reusable PDM audio source.
 *
 * @details Runs the HUM Ch 49.4.1 start flow with the settling delay.
 *          Prints ::k_pdm_demo_banner_fail and parks on any error.
 *
 * @return Nothing.
 *
 * @pre The console and clocks are live.
 * @pre The PDM pins are routed.
 * @post Channel 2 is capturing on success.
 * @post The CPU is parked on any failure.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void pdm_demo_pdm_or_halt(void)
{
  pdm_demo_tx(k_pdm_demo_banner_init, (uint32_t)(sizeof(k_pdm_demo_banner_init) - 1U));
  ra8_board_pdm_mic_config_t board_cfg = {};
  if (ra8_board_pdm_mic_get_config(k_ra8_board_pdm_mic1, &board_cfg) != k_ra8_ok) {
    pdm_demo_tx(k_pdm_demo_banner_fail, (uint32_t)(sizeof(k_pdm_demo_banner_fail) - 1U));
    pdm_demo_panic_halt();
  }
  const ra8_audio_source_pdm_cfg_t source_cfg = {
    .pdm               = board_cfg.pdm,
    .sample_rate_hz    = board_cfg.sample_rate_hz,
    .samples_per_frame = (uint32_t)k_pdm_demo_window,
    .settle_ms         = (uint32_t)k_pdm_demo_settle_ms,
    .discard_samples   = (uint32_t)k_pdm_demo_discard,
    .poll_attempts     = (uint32_t)k_pdm_demo_poll_max,
    .channel           = board_cfg.channel,
    .valid_bits        = board_cfg.valid_bits,
  };
  if (ra8_audio_source_pdm_init(&s_pdm_demo_source, &s_pdm_demo_source_state, &source_cfg) !=
      k_ra8_ok) {
    pdm_demo_tx(k_pdm_demo_banner_fail, (uint32_t)(sizeof(k_pdm_demo_banner_fail) - 1U));
    pdm_demo_panic_halt();
  }
}

/* =============================================================================
 * Capture + analysis
 * =============================================================================
 */

/**
 * @brief Compute min/max/mean/peak/RMS for a captured window.
 *
 * @details Two bounded passes: pass 1 accumulates min/max/mean; pass 2
 *          accumulates the AC peak and mean-square about that mean. A
 *          zero-sample window is rejected before either pass runs, so a
 *          source that reports an empty capture cannot divide by zero.
 *
 * @param[in]  buf Sample buffer (non-NULL).
 * @param[in]  n   Sample count.
 * @param[out] m   Metrics output (non-NULL).
 *
 * @return Nothing.
 *
 * @pre ``buf`` holds ``n`` valid samples.
 * @pre ``m`` is non-NULL.
 * @post ``n == 0`` leaves ``*m`` entirely zeroed.
 * @post ``n > 0`` leaves ``m->rms`` and ``m->peak`` non-negative.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void pdm_demo_analyze(const int32_t* buf, uint32_t n, pdm_metrics_t* m)
{
  if (n == 0U) {
    *m = (pdm_metrics_t){};
    return;
  }
  int64_t sum = 0;
  int32_t lo  = buf[0];
  int32_t hi  = buf[0];
  for (uint32_t i = 0U; i < n; ++i) {
    const int32_t s = buf[i];
    sum += s;
    lo = (s < lo) ? s : lo;
    hi = (s > hi) ? s : hi;
  }
  const int32_t mean = (int32_t)(sum / (int64_t)n);

  uint64_t sumsq = 0U;
  int32_t  peak  = 0;
  for (uint32_t i = 0U; i < n; ++i) {
    const int32_t d   = buf[i] - mean;
    const int32_t mag = (d < 0) ? -d : d;
    peak              = (mag > peak) ? mag : peak;
    sumsq += (uint64_t)((int64_t)d * (int64_t)d);
  }
  m->min  = lo;
  m->max  = hi;
  m->mean = mean;
  m->peak = peak;
  m->rms  = (int32_t)pdm_demo_isqrt(sumsq / (uint64_t)n);
  m->n    = n;
}

/**
 * @brief Decide whether a window carries plausible live audio.
 *
 * @details Non-degenerate (span >= ::k_pdm_demo_min_span) AND carrying AC
 *          energy (rms >= ::k_pdm_demo_rms_floor). Written as nested
 *          single-condition tests (no compound decision).
 *
 * @param[in] m Window metrics (non-NULL).
 *
 * @return ``true`` if the window is plausible live audio.
 * @retval true The span and RMS meet their configured floors.
 * @retval false At least one activity threshold was not met.
 *
 * @pre ``m`` was filled by ::pdm_demo_analyze.
 * @pre ``m->n`` is greater than zero.
 * @post No state is modified.
 * @post The result reflects only ``m``.
 *
 * @note Pure helper; thread-safe.
 * @since 0.1.0
 */
static bool pdm_demo_is_active(const pdm_metrics_t* m)
{
  const int32_t span = m->max - m->min;
  if (span >= (int32_t)k_pdm_demo_min_span) {
    if (m->rms >= (int32_t)k_pdm_demo_rms_floor) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Print a per-window detail line and latch ::g_pdm_mic_result.
 * @details Emits each metric as a bounded decimal field before updating the SWD result object.
 *
 * @param[in] m      Window metrics (non-NULL).
 * @param[in] vary   RMS spread across the report sweep so far.
 * @param[in] active Plausibility verdict for this window.
 *
 * @return Nothing.
 *
 * @pre The console is live.
 * @pre ``m`` was filled by ::pdm_demo_analyze.
 * @post One ``pdm: ...`` detail line was emitted.
 * @post ::g_pdm_mic_result mirrors ``m``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void pdm_demo_publish(const pdm_metrics_t* m, int32_t vary, bool active)
{
  static const uint8_t k_l_rms[]  = "pdm: rms=";
  static const uint8_t k_l_peak[] = " peak=";
  static const uint8_t k_l_mean[] = " mean=";
  static const uint8_t k_l_span[] = " span=";
  static const uint8_t k_l_vary[] = " vary=";
  static const uint8_t k_l_act[]  = " active=";
  static const uint8_t k_l_y[]    = "Y\r\n";
  static const uint8_t k_l_n[]    = "N\r\n";

  pdm_demo_emit_kv(k_l_rms, (uint32_t)(sizeof(k_l_rms) - 1U), m->rms);
  pdm_demo_emit_kv(k_l_peak, (uint32_t)(sizeof(k_l_peak) - 1U), m->peak);
  pdm_demo_emit_kv(k_l_mean, (uint32_t)(sizeof(k_l_mean) - 1U), m->mean);
  pdm_demo_emit_kv(k_l_span, (uint32_t)(sizeof(k_l_span) - 1U), m->max - m->min);
  pdm_demo_emit_kv(k_l_vary, (uint32_t)(sizeof(k_l_vary) - 1U), vary);
  pdm_demo_tx(k_l_act, (uint32_t)(sizeof(k_l_act) - 1U));
  if (active) {
    pdm_demo_tx(k_l_y, (uint32_t)(sizeof(k_l_y) - 1U));
  } else {
    pdm_demo_tx(k_l_n, (uint32_t)(sizeof(k_l_n) - 1U));
  }

  g_pdm_mic_result.rms     = m->rms;
  g_pdm_mic_result.peak    = m->peak;
  g_pdm_mic_result.mean    = m->mean;
  g_pdm_mic_result.span    = m->max - m->min;
  g_pdm_mic_result.vary    = vary;
  g_pdm_mic_result.samples = m->n;
  g_pdm_mic_result.active  = active ? 1U : 0U;
  g_pdm_mic_result.windows = g_pdm_mic_result.windows + 1U;
  g_pdm_mic_result.magic   = (uint32_t)k_pdm_demo_magic;
}

/**
 * @brief Capture one window, analyse it and publish it.
 *
 * @details Reports the no-data banner and returns ``false`` if the FIFO
 *          never fills (dead clock / mic).
 *
 * @param[in]     vary  RMS spread so far (for the detail line).
 * @param[in,out] out_m Receives the window metrics (non-NULL).
 *
 * @return ``true`` if a window was captured and analysed.
 * @retval true Capture, analysis, and publication completed.
 * @retval false The audio source did not provide a frame.
 *
 * @pre The PDM channel is running with data read enabled.
 * @pre ``out_m`` is non-NULL.
 * @post On success ``*out_m`` holds the window statistics.
 * @post ::g_pdm_mic_result is refreshed on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool pdm_demo_run_window(int32_t vary, pdm_metrics_t* out_m)
{
  const ra8_audio_buffer_t buffer = {
    .data     = s_pdm_demo_buf,
    .capacity = (uint32_t)sizeof(s_pdm_demo_buf),
  };
  ra8_audio_frame_t frame = {};
  if (ra8_audio_source_capture(&s_pdm_demo_source, &buffer, &frame) != k_ra8_ok) {
    pdm_demo_tx(k_pdm_demo_banner_nodat, (uint32_t)(sizeof(k_pdm_demo_banner_nodat) - 1U));
    return false;
  }
  pdm_demo_analyze(s_pdm_demo_buf, frame.sample_count, out_m);
  const bool active = pdm_demo_is_active(out_m);
  pdm_demo_publish(out_m, vary, active);
  g_pdm_mic_result.timestamp_ms = frame.timestamp_ms;
  return true;
}

/* =============================================================================
 * Main
 * =============================================================================
 */

/**
 * @brief Application entry: bring up the PDM mic and report plausibility.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU and priority grouping.
 * @post The verdict banner is printed and remains the terminal UART line.
 * @post ::g_pdm_mic_result carries the validated capture result.
 *
 * @since 0.1.0
 */
void main(void)
{
  pdm_demo_clocks_or_halt();
  pdm_demo_io_or_halt();
  pdm_demo_pdm_or_halt();

  pdm_metrics_t m = {};
  /* Sweep windows for the verdict, tracking the RMS spread. */
  int32_t  rms_lo         = INT32_MAX;
  int32_t  rms_hi         = 0;
  uint32_t active_windows = 0U;
  for (uint32_t w = 0U; w < (uint32_t)k_pdm_demo_report_win; ++w) {
    const int32_t vary = (rms_hi > rms_lo) ? (rms_hi - rms_lo) : 0;
    if (!pdm_demo_run_window(vary, &m)) {
      pdm_demo_panic_halt();
    }
    rms_lo = (m.rms < rms_lo) ? m.rms : rms_lo;
    rms_hi = (m.rms > rms_hi) ? m.rms : rms_hi;
    active_windows += (g_pdm_mic_result.active != 0U) ? 1U : 0U;
  }
  if (active_windows == 0U) {
    pdm_demo_tx(k_pdm_demo_banner_inactive, (uint32_t)(sizeof(k_pdm_demo_banner_inactive) - 1U));
    pdm_demo_panic_halt();
  }
  pdm_demo_tx(k_pdm_demo_banner_pass, (uint32_t)(sizeof(k_pdm_demo_banner_pass) - 1U));
  while (1) {
    __asm__ volatile("wfi");
  }

  pdm_demo_panic_halt();
}
