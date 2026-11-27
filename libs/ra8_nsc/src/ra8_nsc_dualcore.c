/**
 * @file ra8_nsc_dualcore.c
 * @brief NSC veneers for dual-core M33 hold-page and M85 sleep handoff
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / NSC] {World: S}
 *
 * @details
 * Implements the Secure TrustZone gateways declared in ``ra8_nsc_dualcore.h``:
 *   - ``ra8_nsc_dualcore_start_m33``: opens SRAM3 (`0x22180000..0x2219FFFF`),
 *     IPC channels 0 and 2, bus masters, and `LED1` (P600) for Non-Secure
 *     access by M33 (CPU1), binds `IPC0` IRQ0 on M85, and releases CPU1.
 *   - ``ra8_nsc_dualcore_park_m85``: temporarily delegates `SW1`/`SW2`
 *     (`P009`/`P008`) to Non-Secure in `PMSAR0`, stops `HOCO`, and enters
 *     Sleep `WFI` in a bounded loop until `IPC0` IRQ0 wakes CPU0, then
 *     restores `HOCO` and returns `SW1`/`SW2` to Secure world.
 *   - ``ra8_nsc_dualcore_notify_cpu1``: signals `IPC1` channel 0 (`ch 2`)
 *     to wake or notify CPU1 after NS updates the mailbox.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_nsc_dualcore.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dual_core.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_ipc.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_sram.h"

/** @brief Diagnostic log tag for dual-core NSC veneers. */
static const char s_tag[] = "NSC_DC";

/** @brief Fixed EK-RA8D2 CPU1 boot and SRAM3 partitioning bounds. */
typedef enum : uintptr_t {
  k_dc_cpu1_mram_base  = 0x020C0000UL, /**< MRAM_CPU1 link base (256 KiB).      */
  k_dc_cpu1_mram_end   = 0x02100000UL, /**< End of MRAM_CPU1 flash partition.   */
  k_dc_sram3_mbox_base = 0x22180000UL, /**< Shared SRAM3 mailbox base.          */
  k_dc_cpu1_sram_lo    = 0x22190000UL, /**< Base of SRAM_CPU1 stack/data bank.  */
  k_dc_cpu1_stack_top  = 0x221A0000UL, /**< Expected top of CPU1 initial stack. */
  k_dc_prcr_s_addr     = 0x4001E3FAUL, /**< SYSC.PRCR_S (16-bit).               */
  k_dc_cpscu_bussara   = 0x40008100UL, /**< CPSCU.BUSSARA (bus security attr).  */
  k_dc_cpscu_bussarb   = 0x40008104UL, /**< CPSCU.BUSSARB (bus security attr).  */
  k_dc_cpscu_bussarc   = 0x40008110UL, /**< CPSCU.BUSSARC (bus security attr).  */
  k_dc_cpscu_mmpusara  = 0x40008130UL, /**< CPSCU.MMPUSARA (master MPU attr).   */
  k_dc_cpscu_mmpusarb  = 0x40008134UL, /**< CPSCU.MMPUSARB (master MPU attr).   */
  k_dc_cpscu_cpusar    = 0x40008170UL, /**< CPSCU.CPUSAR (CPU security attr).   */
  k_dc_cpscu_ipcsar    = 0x40008610UL, /**< CPSCU.IPCSAR (IPC security attr).   */
  k_dc_cpscu_ipcpar    = 0x40008614UL, /**< CPSCU.IPCPAR (IPC privilege attr).  */
  k_dc_pmsar0_addr     = 0x40400D30UL, /**< PMISC.PMSAR0 (16-bit, PORT0 SW1/2). */
  k_dc_pmsar6_addr     = 0x40400D48UL, /**< PMISC.PMSAR6 (16-bit, PORT6 LED1).  */
} dc_reg_addr_t;

/** @brief Register field values, masks, and bounded retry limits. */
typedef enum : uint32_t {
  k_dc_sram3_ns_boundary  = 0x00180000UL, /**< SRAMSABAR3: 0x22180000..0x2219FFFF NS. */
  k_dc_sram_bank_sram3    = 3U,           /**< Bank index for SRAMSABAR3.             */
  k_dc_prcr_open_prc4     = 0xA510U,      /**< Key 0xA500 | PRC4 security attr bit.   */
  k_dc_prcr_lock_all      = 0xA500U,      /**< Key 0xA500 | lock all PRCR groups.     */
  k_dc_bussar_ns_masters  = 0x00000001UL, /**< Bus master attribution for CPU1 NS.    */
  k_dc_mmpusar_all_ns     = 0xFFFFFFFFUL, /**< Allow NS master MPU transactions.      */
  k_dc_cpusar_default     = 0x00000000UL, /**< CPU0 Secure, CPU1 permanent-NS.        */
  k_dc_ipcsar_ch0_ch2_ns  = 0x00050000UL, /**< IPC0 ch0 and IPC1 ch0 Non-Secure.      */
  k_dc_ipcpar_priv_only   = 0x00000000UL, /**< IPC privilege attribution reset value. */
  k_dc_pmsar0_sw1_sw2_msk = 0x0300U,      /**< PMSAR0 bits 9 (SW1) and 8 (SW2).       */
  k_dc_pmsar6_led1_msk    = 0x0001U,      /**< PMSAR6 bit 0 (LED1 P600).              */
  k_dc_ipc_ch_cpu1_to_0   = 0U,           /**< IPC0 channel 0 (M33 -> M85).           */
  k_dc_ipc_ch_cpu0_to_1   = 2U,           /**< IPC1 channel 0 (M85 -> M33).           */
  k_dc_mbox_words         = 12U,          /**< 12 uint32_t words in SRAM3 mailbox.    */
  k_dc_max_wake_iters_cap = 4096U,        /**< NASA Rule 2 cap on Sleep-WFI spins.    */
} dc_cfg_const_t;

/** @brief Latched nonzero by Secure IPC0 IRQ0 callback when M33 pokes M85. */
static volatile uint32_t s_ipc0_wake_flag = 0U;

/** @brief Guard ensuring one-time LPM initialisation across sleep entries. */
static bool s_lpm_ready = false;

/**
 * @brief Unlock PRCR_S security-attribution group PRC4.
 *
 * @details Issues a 16-bit password write (`0xA510`) to `PRCR_S`.
 * @pre Executing in Secure privileged state on CPU0.
 * @post `PRCR_S.PRC4` is open for CPSCU, SRAM, and `PmSAR` writes.
 * @note Not thread-safe; pair strictly with ``internal_dc_prcr_sar_close``.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dc_prcr_sar_open(void)
{
  /* HUM Ch 13.2.1 "PRCR_S" p 521 -- PRC4 unlocks CPSCU, SRAMSABAR, and PmSAR. */
  *(volatile uint16_t*)k_dc_prcr_s_addr = (uint16_t)k_dc_prcr_open_prc4;
  ra8_hw_dsb();
}

/**
 * @brief Relock all PRCR_S write-protect groups.
 *
 * @details Issues a 16-bit password-only write (`0xA500`) to `PRCR_S`.
 * @pre Prior ``internal_dc_prcr_sar_open`` call completed.
 * @post `PRCR_S` write protection is restored across all groups.
 * @note Not thread-safe; pair with ``internal_dc_prcr_sar_open``.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dc_prcr_sar_close(void)
{
  /* HUM Ch 13.2.1 "PRCR_S" p 521 -- key-only write locks PRC0..PRC4. */
  *(volatile uint16_t*)k_dc_prcr_s_addr = (uint16_t)k_dc_prcr_lock_all;
  ra8_hw_dsb();
}

/**
 * @brief Validate CPU1 entry base, stack top, and initial vector words.
 *
 * @details Enforces the fixed EK-RA8D2 `MRAM_CPU1` (`0x020C0000`) and
 * `SRAM_CPU1` (`0x221A0000`) layout, then verifies the vector table carries a
 * valid 8-byte-aligned SP in `SRAM_CPU1` and a Thumb reset address inside
 * `MRAM_CPU1`.
 *
 * @param[in] mram_base Caller-supplied CPU1 image base.
 * @param[in] stack_top Caller-supplied CPU1 initial stack top.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Image bounds and vector table look valid.
 * @retval k_ra8_err_invalid_arg   Base or stack top mismatches layout.
 * @retval k_ra8_err_not_supported Vector table SP or reset PC is blank/invalid.
 *
 * @pre Target `mram_base` memory is readable from CPU0 Secure world.
 * @post No registers modified.
 * @note Thread-safe; read-only check.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dc_validate_cpu1_image(uintptr_t mram_base,
                                                              uintptr_t stack_top)
{
  RA8_CHECK_RANGE((uint64_t)mram_base,
                  (uint64_t)k_dc_cpu1_mram_base,
                  (uint64_t)k_dc_cpu1_mram_base,
                  k_ra8_err_invalid_arg);
  RA8_CHECK_RANGE((uint64_t)stack_top,
                  (uint64_t)k_dc_cpu1_stack_top,
                  (uint64_t)k_dc_cpu1_stack_top,
                  k_ra8_err_invalid_arg);

  const volatile uint32_t* const vec = (const volatile uint32_t*)mram_base;
  const uintptr_t                sp  = (uintptr_t)vec[0U];
  const uintptr_t                rst = (uintptr_t)(vec[1U] & ~(uintptr_t)1UL);
  const bool sp_ok  = (sp > k_dc_cpu1_sram_lo) && (sp <= stack_top) && ((sp & 7UL) == 0UL);
  const bool rst_ok = (rst >= k_dc_cpu1_mram_base) && (rst < k_dc_cpu1_mram_end);
  if (!sp_ok || !rst_ok) {
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

/**
 * @brief Record an IPC0 channel-0 IRQ0 wake signal from CPU1.
 *
 * @details Installed on `IPC0` channel `0` IRQ0 inside the Secure world so a
 * wake pulse from M33 releases `ra8_nsc_dualcore_park_m85`.
 *
 * @param[in] ctx      Unused context pointer.
 * @param[in] channel  IPC channel index (expected 0).
 * @param[in] event_id IPC IRQ event line (expected 0).
 *
 * @pre Registered via ``ra8_ipc_attach_event_handler``.
 * @post ``s_ipc0_wake_flag`` set to 1.
 * @note ISR-safe; performs a single volatile store.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_dc_ipc0_wake_cb(void* ctx, uint8_t channel, ra8_ipc_irq_event_id_t event_id)
{
  (void)ctx;
  (void)channel;
  (void)event_id;
  s_ipc0_wake_flag = 1U;
}

/**
 * @brief Initialise IPC channels 0/2 and install the Secure IPC0 wake ISR.
 *
 * @details Resets and clears `IPC0` (`ch 0`, CPU1->CPU0) and `IPC1` (`ch 2`,
 * CPU0->CPU1), attaches ``internal_dc_ipc0_wake_cb``, and wires NVIC IRQ0.
 *
 * @return ::ra8_err_t outcome from HAL IPC setup.
 * @retval k_ra8_ok IPC channels and Secure IRQ0 wake path ready.
 *
 * @pre Call before moving `IPCSAR` channel 0 and 2 to Non-Secure.
 * @post `IPC0` IRQ0 fires ``internal_dc_ipc0_wake_cb`` when M33 pokes `IPC0`.
 * @note Not thread-safe; called once during dual-core bring-up.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dc_arm_ipc(void)
{
  const ra8_ipc_config_t rx_cfg = {
    .channel      = (uint8_t)k_dc_ipc_ch_cpu1_to_0,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_irq0,
  };
  const ra8_ipc_config_t tx_cfg = {
    .channel      = (uint8_t)k_dc_ipc_ch_cpu0_to_1,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_none,
  };

  ra8_err_t err = ra8_ipc_init(&rx_cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_ipc_init(&tx_cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_ipc_attach_event_handler((uint8_t)k_dc_ipc_ch_cpu1_to_0,
                                     k_ra8_ipc_irq_event_0,
                                     internal_dc_ipc0_wake_cb,
                                     nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_ipc_install_isr(k_ra8_ipc_unit_ipc0, k_ra8_isr_prio_default);
}

/**
 * @brief Partition SRAM3, CPSCU IPC channels, and LED1 attribution for CPU1.
 *
 * @details Opens `PRCR_S.PRC4`, sets `SRAMSABAR3` to `0x00180000` so shared
 * mailbox (`0x22180000`) and `SRAM_CPU1` (`0x22190000..0x2219FFFF`) are NS,
 * grants NS bus/MPU attribution plus `IPCSAR = 0x00050000` for channels 0 and 2,
 * hands `LED1` (`P600`) to NS in `PMSAR6`, and closes `PRCR_S`.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok Dual-core TrustZone security split applied.
 *
 * @pre Executing in CPU0 Secure privileged mode.
 * @post `SRAMSABAR3`, `CPSCU`, and `PMSAR6` configured; `PRCR_S` relocked.
 * @note Not thread-safe; called once during CPU1 bring-up.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dc_configure_security(void)
{
  internal_dc_prcr_sar_open();

  const ra8_err_t err =
    ra8_sram_set_boundary((uint8_t)k_dc_sram_bank_sram3, (uint32_t)k_dc_sram3_ns_boundary);
  if (err == k_ra8_ok) {
    /* HUM Ch 15.3.1 "BUSSARA" p 591 -- assign CPU1 master bus traffic to NS. */
    *(volatile uint32_t*)k_dc_cpscu_bussara = (uint32_t)k_dc_bussar_ns_masters;
    /* HUM Ch 15.3.2 "BUSSARB" p 592 -- assign CPU1 master bus traffic to NS. */
    *(volatile uint32_t*)k_dc_cpscu_bussarb = (uint32_t)k_dc_bussar_ns_masters;
    /* HUM Ch 15.3.3 "BUSSARC" p 592 -- assign CPU1 master bus traffic to NS. */
    *(volatile uint32_t*)k_dc_cpscu_bussarc = (uint32_t)k_dc_bussar_ns_masters;
    /* HUM Ch 2.9.1.1 "CPUSAR" p 126 -- keep CPU0 Secure and CPU1 permanent-NS. */
    *(volatile uint32_t*)k_dc_cpscu_cpusar = (uint32_t)k_dc_cpusar_default;
    /* HUM Ch 16.3.1.1 "MMPUSARA" p 694 -- allow Non-Secure master MPU access. */
    *(volatile uint32_t*)k_dc_cpscu_mmpusara = (uint32_t)k_dc_mmpusar_all_ns;
    /* HUM Ch 16.3.1.2 "MMPUSARB" p 695 -- allow Non-Secure master MPU access. */
    *(volatile uint32_t*)k_dc_cpscu_mmpusarb = (uint32_t)k_dc_mmpusar_all_ns;
    /* HUM Ch 3.2.1 "IPCSAR" p 205 -- assign IPC0 ch0 and IPC1 ch0 to NS. */
    *(volatile uint32_t*)k_dc_cpscu_ipcsar = (uint32_t)k_dc_ipcsar_ch0_ch2_ns;
    /* HUM Ch 3.2.2 "IPCPAR" p 208 -- privileged-only access on IPC channels. */
    *(volatile uint32_t*)k_dc_cpscu_ipcpar = (uint32_t)k_dc_ipcpar_priv_only;

    /* HUM Ch 20.2.9 "PmSAR" p 848 -- 16-bit access ONLY: hand P600 (LED1) to NS. */
    volatile uint16_t* const pmsar6 = (volatile uint16_t*)k_dc_pmsar6_addr;
    *pmsar6                         = (uint16_t)(*pmsar6 | (uint16_t)k_dc_pmsar6_led1_msk);
  }

  internal_dc_prcr_sar_close();
  return err;
}

/**
 * @brief Delegate or reclaim SW1 (`P009`) and SW2 (`P008`) Non-Secure ownership.
 *
 * @details When `allow_ns` is true, sets `PMSAR0` bits 8 and 9 so M33 can poll
 * switches while M85 is parked; when false, clears those bits back to Secure so
 * `ra8_nsc_sw_read` owns switches cleanly during active UI turns.
 *
 * @param[in] allow_ns `true` to grant NS ownership, `false` to restore Secure.
 *
 * @pre Executing in CPU0 Secure privileged mode.
 * @post `PMSAR0[9:8]` matches `allow_ns`; `PRCR_S` relocked.
 * @note Not thread-safe; serialised around M85 sleep entry/exit.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_dc_set_switches_ns(bool allow_ns)
{
  internal_dc_prcr_sar_open();

  /* HUM Ch 20.2.9 "PmSAR" p 848 -- 16-bit read/write under PRCR protection. */
  volatile uint16_t* const pmsar0 = (volatile uint16_t*)k_dc_pmsar0_addr;
  uint16_t                 val    = *pmsar0;
  if (allow_ns) {
    val = (uint16_t)(val | (uint16_t)k_dc_pmsar0_sw1_sw2_msk);
  } else {
    val = (uint16_t)(val & (uint16_t)~((uint16_t)k_dc_pmsar0_sw1_sw2_msk));
  }
  *pmsar0                  = val;
  const uint16_t confirmed = *pmsar0;
  (void)confirmed;

  internal_dc_prcr_sar_close();
}

/**
 * @brief Initialise the LPM driver in Sleep-ready mode once.
 *
 * @details Unlocks `PRCR.PRC1`, configures `ra8_lpm_init` with
 * `opa_bus_keep = true`, and re-locks `PRCR.PRC1`.
 *
 * @return ::ra8_err_t outcome from LPM initialisation.
 * @retval k_ra8_ok LPM ready for HOCO control and Sleep `WFI`.
 *
 * @pre Executing in CPU0 Secure privileged mode.
 * @post ``s_lpm_ready`` is true on success; `PRCR.PRC1` relocked.
 * @note Not thread-safe; single coordinator path only.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dc_ensure_lpm_ready(void)
{
  if (s_lpm_ready) {
    return k_ra8_ok;
  }
  ra8_err_t err = ra8_lpm_prcr_unlock();
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_lpm_config_t cfg = {
    .opa_bus_keep   = true,
    .dcdc_softstart = k_ra8_lpm_dcssmode_128us,
  };
  err = ra8_lpm_init(&cfg);
  (void)ra8_lpm_prcr_relock();
  if (err == k_ra8_ok) {
    s_lpm_ready = true;
  }
  return err;
}

/**
 * @brief Set or clear HOCO clock-stop (`HOCOCR.HCSTP`) under PRCR protection.
 *
 * @details Opens `PRCR`, calls ``ra8_lpm_set_clock_stop(k_ra8_lpm_clock_hoco,
 * stop)``, and re-locks `PRCR`.
 *
 * @param[in] stop `true` to stop HOCO during Sleep, `false` to resume it.
 *
 * @return ::ra8_err_t outcome from ``ra8_lpm_set_clock_stop``.
 * @retval k_ra8_ok `HOCOCR.HCSTP` updated.
 *
 * @pre ``internal_dc_ensure_lpm_ready`` succeeded.
 * @post `PRCR` is re-locked on every return path.
 * @note Not thread-safe; single coordinator path only.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dc_set_hoco_stop(bool stop)
{
  ra8_err_t err = ra8_lpm_prcr_unlock();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_lpm_set_clock_stop(k_ra8_lpm_clock_hoco, stop);
  (void)ra8_lpm_prcr_relock();
  return err;
}

[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dualcore_start_m33(uintptr_t cpu1_mram_base,
                                                                  uintptr_t cpu1_stack_top)
{
  ra8_err_t err = internal_dc_validate_cpu1_image(cpu1_mram_base, cpu1_stack_top);
  if (err != k_ra8_ok) {
    return err;
  }

  err = internal_dc_arm_ipc();
  if (err != k_ra8_ok) {
    return err;
  }

  err = internal_dc_configure_security();
  if (err != k_ra8_ok) {
    return err;
  }

  volatile uint32_t* const mbox = (volatile uint32_t*)k_dc_sram3_mbox_base;
  RA8_LOOP_BOUND(k_dc_mbox_words);
  for (uint32_t i = 0U; i < (uint32_t)k_dc_mbox_words; ++i) {
    mbox[i] = 0U;
  }

  s_ipc0_wake_flag = 0U;
  ra8_hw_dsb();
  return ra8_cpu1_release((void*)cpu1_mram_base, (void*)cpu1_stack_top);
}

[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dualcore_park_m85(uint32_t  max_wake_iters,
                                                                 uint32_t* out_woken)
{
  RA8_CHECK_NULL_PTR(out_woken, s_tag, "park_m85: out_woken");
  RA8_CHECK_RANGE(max_wake_iters, 1U, (uint32_t)k_dc_max_wake_iters_cap, k_ra8_err_invalid_arg);
  RA8_NSC_CHECK_NS_RANGE_RW(out_woken, sizeof(*out_woken));

  *out_woken    = 0U;
  ra8_err_t err = internal_dc_ensure_lpm_ready();
  if (err != k_ra8_ok) {
    return err;
  }

  internal_dc_set_switches_ns(true);
  const ra8_err_t stop_err = internal_dc_set_hoco_stop(true);
  if (stop_err == k_ra8_ok) {
    err = ra8_lpm_prcr_unlock();
    if (err == k_ra8_ok) {
      for (uint32_t iter = 0U; iter < max_wake_iters; ++iter) {
        if (s_ipc0_wake_flag != 0U) {
          *out_woken       = 1U;
          s_ipc0_wake_flag = 0U;
          break;
        }
        err = ra8_lpm_enter_sleep(k_ra8_sleep_mode_sleep);
        if (err != k_ra8_ok) {
          break;
        }
        if (s_ipc0_wake_flag != 0U) {
          *out_woken       = 1U;
          s_ipc0_wake_flag = 0U;
          break;
        }
      }
      (void)ra8_lpm_prcr_relock();
    }
    (void)internal_dc_set_hoco_stop(false);
  } else {
    err = stop_err;
  }

  internal_dc_set_switches_ns(false);
  return err;
}

[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_dualcore_notify_cpu1(void)
{
  ra8_hw_dsb();
  return ra8_ipc_send_event((uint8_t)k_dc_ipc_ch_cpu0_to_1, k_ra8_ipc_irq_event_0);
}
