/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_cdc/main.c
 * @brief USB self-loop: HS host writes bytes to a CDC-ACM device, reads the echo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The CDC-ACM echo self-loop -- it exercises a bidirectional bulk
 * round-trip through a real device class (host bulk-OUT -> device
 * receives -> device echoes -> host bulk-IN). The two USB ports are
 * cabled to EACH OTHER and one firmware image runs both USB stacks:
 *
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX CDC-ACM class. A worker
 *    loops `_ux_device_class_cdc_acm_read` -> `_ux_device_class_cdc_acm_write`,
 *    echoing every byte the host sends straight back on the bulk-IN
 *    endpoint. IRQ-driven through the `port/usbx/ux_dcd_ra8_usb` bridge.
 *    (Worker-thread echo, NOT the DCD ISR auto-echo -- the worker path
 *    rides the normal device bulk-OUT receive that the WRITE(10) driver
 *    fix repaired.)
 *  - USBHS (J7) = HOST: a self-contained polled CDC host built on the
 *    first-party `ra8_usb_host_*` primitives. It enumerates the device
 *    (bus reset -> GET_DESCRIPTOR -> SET_ADDRESS -> SET_CONFIGURATION),
 *    opens the CDC data interface's bulk pipes (EP2 OUT / EP1 IN), then
 *    runs several rounds: bulk-OUT a deterministic pattern, bulk-IN the
 *    echo, and byte-check it -- proving the bidirectional bulk path
 *    round-trips intact, end to end on chip.
 *
 * Each round ships a sub-MPS (60-byte) payload so the echo returns as a
 * single short packet (no MPS-exact ZLP ambiguity on the host bulk-IN).
 * No serial terminal is involved; raw bulk transfers only.
 *
 * The link runs at 12 Mbps (FS device ceiling; HS host serves an FS
 * downstream device).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "usb_selftest_cdc_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra8_time millisecond
 * counter (for ra8_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer; the 1 ms pulse also recovers the DCD's storm-guard mask. */

extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick before tx_kernel_enter and the setup
 *          window is long (U15 expander I2C blocks for ms), so the tick
 *          fires pre-kernel; feeding _tx_timer_interrupt into ThreadX's
 *          zeroed timer state bus-faults. Gate it until the kernel runs.
 * @since 0.1.0
 */
static volatile bool s_tx_kernel_up = false;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  ra8_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra8_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual) */
/* -------------------------------------------------------------------------- */

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra8_port_pin_t k_cdc_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_cdc_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_cdc_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_cdc_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_cdc_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra8_port_pin_t k_cdc_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/* The shared sizing/geometry enums (::cdc_config_t, ::cdc_geom_t) live in the
 * companion `usb_selftest_cdc_steps.h`; the host-only formatter, mask, and
 * phase enums live in `usb_selftest_cdc_steps.c`. Only the device-side step
 * enum is private to this unit. */

/**
 * @enum cdc_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_cdc_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_cdc_dev_step_class  = 2U, /**< CDC-ACM class registered.      */
  k_cdc_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_cdc_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_cdc_dev_step_echo   = 5U, /**< Echo loop running.             */
} cdc_dev_step_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage */
/* -------------------------------------------------------------------------- */

/**
 * @var s_device_thread
 * @brief ThreadX TCB for the USBX device-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_device_thread;

/**
 * @var s_device_stack
 * @brief Stack backing storage for ::s_device_thread.
 * @since 0.1.0
 */
static UCHAR s_device_stack[k_cdc_thread_stack];

/**
 * @var s_host_thread
 * @brief ThreadX TCB for the host-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_host_thread;

/**
 * @var s_host_stack
 * @brief Stack backing storage for ::s_host_thread.
 * @since 0.1.0
 */
static UCHAR s_host_stack[k_cdc_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_cdc_usbx_pool_bytes];

/**
 * @var s_cdc_acm
 * @brief Active CDC-ACM class instance, captured by the activate callback.
 * @note Written by the USBX class thread; read by the device echo worker.
 * @since 0.1.0
 */
static UX_SLAVE_CLASS_CDC_ACM* s_cdc_acm = UX_NULL;

/**
 * @var s_cdc_active_sem
 * @brief Posted by the activate callback so the echo worker blocks on it
 *        instead of polling ``s_cdc_acm`` with tx_thread_sleep (which has
 *        been observed never returning on this silicon under load).
 * @note Single-producer (class thread), single-consumer (echo worker).
 * @since 0.1.0
 */
static TX_SEMAPHORE s_cdc_active_sem;

/* -------------------------------------------------------------------------- */
/* J-Link probes (device side) */
/* -------------------------------------------------------------------------- */

/* The host-side ladder probes (``s_dbg_phase``, ``s_dbg_rounds_ok``,
 * ``s_dbg_pid``, ``s_dbg_mismatch``, ``s_dbg_pass_count``) live in the
 * companion `usb_selftest_cdc_steps.c` alongside their only writers. */

/** @brief Device-side echo iterations (one read+write each). */
static volatile uint32_t s_dbg_dev_echo_calls;
/** @brief Bytes the device echoed on the most recent round. */
static volatile uint32_t s_dbg_dev_last_len;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 echo. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/* -------------------------------------------------------------------------- */
/* USB descriptors (CDC-ACM: comm + data interface via an IAD) */
/* -------------------------------------------------------------------------- */

/* CDC-ACM config: one communications interface + one data interface joined
 * by an Interface Association Descriptor, EP3 IN (interrupt notify), EP2 OUT
 * / EP1 IN (bulk data, 64-byte MPS). Layout per CDC 1.20 sec 5 + USB 2.0 sec
 * 9.6 -- byte-identical to the proven usb_cdc_echo device, retagged PID
 * 0x0017 for the CDC self-test identity. The host only drives the bulk data
 * pipes (EP2 OUT / EP1 IN); the interrupt-IN notify endpoint is unused. */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. bcdUSB 0x0200 and
     class MISC/common/IAD so IAD-based composites enumerate cleanly. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0xEFU, /* class      = MISC   */
  0x02U, /* subclass   = common */
  0x01U, /* protocol   = IAD    */
  0x40U,
  0x09U,
  0x12U,
  0x17U, /* PID = 0x0017 (pid.codes test). */
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (75 bytes total = 0x4B). */
  0x09U,
  0x02U,
  0x4BU,
  0x00U,
  0x02U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface association (CDC). */
  0x08U,
  0x0BU,
  0x00U,
  0x02U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* Communications interface (CDC ACM). */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* CDC header functional descriptor. bcdCDC = 0x0120. */
  0x05U,
  0x24U,
  0x00U,
  0x20U,
  0x01U,
  /* Call-management functional descriptor. */
  0x05U,
  0x24U,
  0x01U,
  0x01U,
  0x01U,
  /* ACM functional descriptor. */
  0x04U,
  0x24U,
  0x02U,
  0x02U,
  /* Union functional descriptor. */
  0x05U,
  0x24U,
  0x06U,
  0x00U,
  0x01U,
  /* Interrupt-IN endpoint (EP3 IN, 8-byte MPS, 255 ms poll). */
  0x07U,
  0x05U,
  0x83U,
  0x03U,
  0x08U,
  0x00U,
  0xFFU,
  /* Data-class interface. */
  0x09U,
  0x04U,
  0x01U,
  0x00U,
  0x02U,
  0x0AU,
  0x00U,
  0x00U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT, 64-byte MPS). */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
  /* Bulk-IN endpoint (EP1 IN, 64-byte MPS). */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
};

/**
 * @var s_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @details Each entry: 2 bytes lang-id, 1 byte string index, 1 byte
 *          length, then ASCII bytes.
 * @since 0.1.0
 */
static UCHAR s_string_framework[] = {
  /* idx 1: "Brighton Sikarskie". */
  0x09U,
  0x04U,
  0x01U,
  0x12U,
  'B',
  'r',
  'i',
  'g',
  'h',
  't',
  'o',
  'n',
  ' ',
  'S',
  'i',
  'k',
  'a',
  'r',
  's',
  'k',
  'i',
  'e',
  /* idx 2: "RA8D2 CDC ECHO". */
  0x09U,
  0x04U,
  0x02U,
  0x0EU,
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'C',
  'D',
  'C',
  ' ',
  'E',
  'C',
  'H',
  'O',
  /* idx 3: serial. */
  0x09U,
  0x04U,
  0x03U,
  0x08U,
  '0',
  '0',
  '0',
  '0',
  '0',
  '0',
  '1',
  '3',
};

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* CDC-ACM activate / deactivate callbacks */
/* -------------------------------------------------------------------------- */

/**
 * @brief CDC-ACM activate callback. Captures the live class instance.
 *
 * @details Pins ``ux_slave_device_state`` at CONFIGURED (works around a
 * residual DVSQ-poll race on this silicon that can demote it back to
 * ATTACHED after SET_CONFIGURATION and break the cdc_acm_read gate), then
 * posts the semaphore the echo worker blocks on. The DCD ISR auto-echo is
 * deliberately NOT enabled -- the worker read/write path is the sole data
 * mover, so the two cannot race on the bulk-OUT pipe.
 *
 * @param[in] cdc_instance Pointer to ``UX_SLAVE_CLASS_CDC_ACM``.
 *
 * @pre Called from the USBX class thread.
 * @pre SET_CONFIGURATION has just configured the device.
 * @post ``s_cdc_acm`` points at the live class.
 * @post ::s_cdc_active_sem is posted so the echo worker runs.
 *
 * @note USBX serializes this with the deactivate callback.
 * @since 0.1.0
 */
static VOID cdc_activate(VOID* cdc_instance)
{
  s_cdc_acm = (UX_SLAVE_CLASS_CDC_ACM*)cdc_instance;
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  (void)tx_semaphore_put(&s_cdc_active_sem);
}

/**
 * @brief CDC-ACM deactivate callback. Drops the live class pointer.
 *
 * @param[in] cdc_instance Unused.
 *
 * @pre Called from the USBX class thread.
 * @pre The CDC-ACM class is being torn down.
 * @post ``s_cdc_acm`` is ``UX_NULL``.
 * @post The echo worker blocks until the next activate.
 *
 * @note USBX serializes this with the activate callback.
 * @since 0.1.0
 */
static VOID cdc_deactivate(VOID* cdc_instance)
{
  (void)cdc_instance;
  s_cdc_acm = UX_NULL;
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX CDC-ACM echo */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bring USBX system + device stack up with the CDC-ACM framework.
 *
 * @details One-shot USBX pool + device-stack init (FS-only framework).
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope pool reserved.
 * @pre Thread context.
 * @post Device stack accepts class registrations.
 * @post On failure USBX state is undefined.
 *
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT cdc_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_cdc_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     s_device_framework_fs,
                                     sizeof(s_device_framework_fs),
                                     s_string_framework,
                                     sizeof(s_string_framework),
                                     s_language_id_framework,
                                     sizeof(s_language_id_framework),
                                     UX_NULL);
}

/**
 * @brief Register the CDC-ACM class against configuration 1, interface 0.
 *
 * @details Binds ::cdc_activate / ::cdc_deactivate so the worker learns
 * when the host has configured the device. The comm + data interfaces are
 * described by the device framework (IAD composite).
 *
 * @return UINT UX_SUCCESS on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::cdc_usbx_stack_up has succeeded.
 * @pre ::cdc_activate / ::cdc_deactivate are defined.
 * @post The CDC-ACM class is bound; the activate callback will fire on
 *       SET_CONFIGURATION.
 * @post No other class is registered.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT cdc_class_register(void)
{
  UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_params = {
    .ux_slave_class_cdc_acm_instance_activate   = cdc_activate,
    .ux_slave_class_cdc_acm_instance_deactivate = cdc_deactivate,
    .ux_slave_class_cdc_acm_parameter_change    = UX_NULL,
  };
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_cdc_acm",
                                         _ux_device_class_cdc_acm_entry,
                                         1,
                                         0,
                                         &cdc_params);
}

/**
 * @brief One device echo iteration: read a bulk-OUT chunk, write it back.
 *
 * @details Pins CONFIGURED (DVSQ-poll race guard), then
 * `_ux_device_class_cdc_acm_read` (blocks for the host's bulk-OUT, returns
 * on the short packet) -> `_ux_device_class_cdc_acm_write` (stages the same
 * bytes on bulk-IN). This rides the normal device bulk-OUT receive path the
 * WRITE(10) driver fix repaired, NOT the DCD ISR auto-echo.
 *
 * @param[in,out] buf Scratch buffer for one chunk.
 * @param[in]     cap Capacity of @p buf in bytes.
 *
 * @pre ``s_cdc_acm`` is non-NULL (class activated).
 * @pre @p buf holds @p cap bytes.
 * @post On a non-empty read the bytes were echoed; counters advanced.
 * @post On a read error the worker backs off one idle tick.
 *
 * @note Runs on the device worker thread.
 * @since 0.1.0
 */
static void cdc_echo_iter(UCHAR* buf, ULONG cap)
{
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  ULONG n = 0UL;
  if (_ux_device_class_cdc_acm_read(s_cdc_acm, buf, cap, &n) != UX_SUCCESS) {
    tx_thread_sleep(1U);
    return;
  }
  if (n == 0UL) {
    return;
  }
  if (_ux_device_class_cdc_acm_write(s_cdc_acm, buf, n, &n) != UX_SUCCESS) {
    return;
  }
  s_dbg_dev_echo_calls++;
  s_dbg_dev_last_len = (uint32_t)n;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
}

/**
 * @brief Device-side worker: bring the CDC-ACM device up, then echo forever.
 *
 * @details USBX system + device stack + CDC-ACM class + DCD bridge on the
 * USBFS controller, then DPRPU attach. Blocks on ::s_cdc_active_sem until
 * the host configures the device, then loops ::cdc_echo_iter.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and echoes bulk-OUT back on bulk-IN.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID cdc_device_worker(ULONG arg)
{
  (void)arg;

  UINT ux = cdc_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_stack;
  ux             = cdc_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_class;
  ra8_err_t e    = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_dcd;
  e              = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_attach;

  static UCHAR s_echo_buf[k_cdc_echo_buf];
  while (1) {
    if (s_cdc_acm == UX_NULL) {
      (void)tx_semaphore_get(&s_cdc_active_sem, TX_WAIT_FOREVER);
      continue;
    }
    s_dbg_dev_step = (uint32_t)k_cdc_dev_step_echo;
    cdc_echo_iter(s_echo_buf, (ULONG)k_cdc_echo_buf);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Creates the activation semaphore, then the device worker at
 * priority 8 and the host worker at 24 (below the USBX class threads).
 * Sets ::s_tx_kernel_up so SysTick may feed ThreadX from here on.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post ::s_cdc_active_sem exists and two auto-start workers are queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_semaphore_create(&s_cdc_active_sem, (CHAR*)"cdc_active", 0U);
  (void)tx_thread_create(&s_device_thread,
                         "cdc_device",
                         cdc_device_worker,
                         0UL,
                         s_device_stack,
                         k_cdc_thread_stack,
                         (UINT)k_cdc_dev_priority,
                         (UINT)k_cdc_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         "cdc_host",
                         cdc_host_worker,
                         0UL,
                         s_host_stack,
                         k_cdc_host_stack,
                         (UINT)k_cdc_host_priority,
                         (UINT)k_cdc_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

/* -------------------------------------------------------------------------- */
/* Startup */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @details Last-resort stop; only a debugger or reset recovers.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void cdc_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route both ports' pins: FS as device, HS as host.
 *
 * @details FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (else
 * peripheral routing forces host VBUSEN and blocks device enum),
 * P8_14/P8_15 data. HS host: SW4-8 to Host via the U15 expander, PD07
 * HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::cdc_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void cdc_route_usb_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_cdc_pin_fs_vbus, k_ra8_psel_usb_fs, "cdc.fs_vbus") != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_gpio_output_init(k_cdc_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_cdc_pin_fs_dp, k_ra8_psel_usb_fs, "cdc.fs_dp") != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_cdc_pin_fs_dm, k_ra8_psel_usb_fs, "cdc.fs_dm") != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_gpio_output_init(k_cdc_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_cdc_pin_hs_vbus, k_ra8_psel_usb_hs, "cdc.hs_vbus") != k_ra8_ok) {
    cdc_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference; USBHS needs its UTMI
 * PLL. SCI8 is the J-Link OB CDC console at 115200.
 *
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 *
 * @note Panic-halts on any failure; called once from main.
 * @since 0.1.0
 */
static void cdc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_cdc_baud) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    cdc_panic_halt();
  }
  cdc_route_usb_or_halt();
}

/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the workers only deal with stack bring-up.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  cdc_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  cdc_panic_halt();
}
