/**
 * @file port/usbx/src/ux_dcd_ra8_usb.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- lifecycle + storm guard.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Implements the dispatch contract documented in ``ux_dcd_ra8_usb.h``.
 * Mirrors the layout of upstream USBX DCD ports (e.g.
 * ``ux_dcd_sim_slave_function``) but routes every call through the
 * project's ``ra8_usb_*`` register-level driver instead of touching
 * USB controller registers directly.
 *
 * This translation unit owns the bridge singleton (``s_dcd`` / ``s_diag``),
 * the lifecycle entry points (``ux_dcd_ra8_usb_initialize`` /
 * ``_uninitialize`` / ``_state``), and the USBFS interrupt-storm guard.
 * The per-aspect siblings (``ux_dcd_ra8_usb_{ep,xfer,isr,setup,dvst,dvst_default,irq}.c``)
 * share state and helpers through ``ux_dcd_ra8_usb_internal.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include "ux_dcd_ra8_usb.h"

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_elc_regs.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_usb_regs.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb_internal.h"
#include "ux_device_stack.h"
#include "ux_system.h"
#include "ux_utility.h"

/**
 * @enum ra8_usb_dcd_isr_prio_t
 * @brief NVIC priority chosen for the USB controller IRQs.
 *
 * @details
 * USB completion IRQs sit between SysTick (priority 0, the highest in
 * this firmware) and the application-level work threads. Picking 4
 * leaves headroom for higher-priority drivers (timers, fault paths)
 * while still pre-empting ThreadX context switches and USBX class
 * threads so SETUP / BRDY / BEMP events drain promptly.
 */
typedef enum : uint8_t {
  /* Priority 8 (NVIC 0x80) leaves SysTick (NVIC 0x40) and PendSV able to
   * preempt the USB ISR. With same priority, SysTick cannot interrupt the
   * USB ISR and the 8 kHz SOFR + per-pipe NRDY events starve thread mode,
   * preventing tx_thread_sleep from ever waking. HUM Ch 13 NVIC priorities. */
  k_ra8_usb_dcd_isr_prio = 8U, /**< RA8 USB dcd ISR prio. */
} ra8_usb_dcd_isr_prio_t;

/* Tag used by ra8_log_*. Must be a static lifetime string. */
static const char* const s_tag = "ux_dcd_ra8_usb";

/**
 * @var s_syscfg_after_dcd_init
 * @brief SYSCFG snapshot at end of ux_dcd_ra8_usb_initialize.
 *
 * @details Bisect probe for the "USBE clears between phy bring-up and
 * echo loop" regression. Read via JLink to confirm whether the DCD
 * bridge or anything it calls (e.g. _ux_dcd_ra8_usb_function
 * CREATE_ENDPOINT, _ux_utility_descriptor_parse, the dispatch worker
 * spawn) clobbers SYSCFG. HUM Ch 37.2.1 SYSCFG p 2060.
 *
 * @note Read-only from outside; written only by ::ux_dcd_ra8_usb_initialize.
 * @since 0.1.0
 */
volatile uint16_t s_syscfg_after_dcd_init = 0U;

/**
 * @var s_lpsts_after_dcd_init
 * @brief LPSTS snapshot at end of ux_dcd_ra8_usb_initialize.
 *
 * @details Companion bisect probe; expected SUSPENDM=1 (0x4000). HUM
 * Ch 37.2.43 LPSTS p 2111.
 *
 * @note Read-only from outside; written only by ::ux_dcd_ra8_usb_initialize.
 * @since 0.1.0
 */
volatile uint16_t s_lpsts_after_dcd_init = 0U;

/**
 * @var s_dcd
 * @brief The single bridge instance. RA8 has two USB controllers
 * but the device stack only ever drives one at a time, so a single
 * static is sufficient.
 *
 * @note Not thread-safe -- updated from the ISR and the dispatch
 * trampoline; concurrency must be arbitrated at the call-site.
 */
ra8_usb_dcd_t s_dcd = {
  .state = k_ux_dcd_ra8_usb_state_uninit,
  .speed = k_ra8_usb_speed_fs,
  .owner = nullptr,
  .pipes = {},
};

volatile uint32_t s_dcd_auto_echo_enable = 0U;

uint8_t s_dcd_auto_echo_out_pipe = 0U;

uint8_t s_dcd_auto_echo_in_pipe = 0U;

/**
 * @brief Enable bridge-side auto-echo on the configured pipe pair.
 *
 * @details All data arriving on ``out_pipe`` (bulk OUT) is drained and
 * re-queued onto ``in_pipe`` (bulk IN) from inside the IRQ path.
 * Workaround for ThreadX worker thread scheduling failure on this
 * silicon. Once enabled, the loop runs whenever
 * ::internal_irq_walk_pipe encounters an OUT pipe with no waiter; see
 * ::internal_irq_auto_echo for the body.
 *
 * @param[in] out_pipe Pipe index to drain on (bulk OUT).
 * @param[in] in_pipe  Pipe index to re-queue on (bulk IN).
 *
 * @return No value; the call cannot fail.
 * @note Auto-echo is enabled on the requested pipe pair.
 *
 * @pre Both pipes are configured via ::ra8_usb_configure_endpoint.
 * @pre Called from task / startup context (not from inside an ISR).
 * @post ``s_dcd_auto_echo_enable == 1``.
 * @post ``s_dcd_auto_echo_out_pipe == out_pipe`` and
 *       ``s_dcd_auto_echo_in_pipe == in_pipe``.
 *
 * @note Not thread-safe; intended for one-shot configuration at startup.
 * @since 0.1.0
 */
void ux_dcd_ra8_usb_auto_echo_enable(uint8_t out_pipe, uint8_t in_pipe)
{
  s_dcd_auto_echo_out_pipe = out_pipe;
  s_dcd_auto_echo_in_pipe  = in_pipe;
  s_dcd_auto_echo_enable   = 1U;
}

/**
 * @enum ra8_nvic_reg_t
 * @brief Cortex-M NVIC set/clear-enable register array base addresses.
 */
typedef enum : uintptr_t {
  k_nvic_iser_base = 0xE000E100U, /**< NVIC Interrupt Set-Enable Register array.   */
  k_nvic_icer_base = 0xE000E180U, /**< NVIC Interrupt Clear-Enable Register array. */
  k_nvic_ispr_base = 0xE000E200U, /**< NVIC Interrupt Set-Pending Register array.  */
} ra8_nvic_reg_t;

/**
 * @enum ra8_nvic_layout_t
 * @brief Bit layout of the NVIC ISER/ICER register array.
 */
typedef enum : uint32_t {
  k_nvic_irqs_per_reg = 32U, /**< IRQ lines covered by one ISER/ICER word.         */
  k_nvic_reg_stride   = 4U,  /**< Byte stride between consecutive ISER/ICER words. */
} ra8_nvic_layout_t;

/**
 * @var s_usb_irq_slot
 * @brief NVIC slot the USB controller's ELC event is routed to.
 * @details Resolved once via ::ra8_isr_lookup_slot in
 *          ::internal_usbfs_storm_guard_init; drives the ISER/ICER math.
 * @note Written once at init; read from ISR / timer context.
 * @since 0.1.0
 */
static uint16_t s_usb_irq_slot = 0U;

/**
 * @var s_isr_spurious_run
 * @brief Consecutive event-less FS ISR entries since the last SysTick re-enable.
 * @details Incremented by ::internal_usbfs_isr on an entry with no real
 *          event bit, reset by it on a real event, and zeroed every 1 ms by
 *          ::ux_dcd_ra8_usb_irq_reenable. JLink-readable storm probe.
 * @since 0.1.0
 */
volatile uint32_t s_isr_spurious_run = 0U;

/**
 * @brief Set or clear the USB controller's NVIC enable bit.
 *
 * @details Indexes the NVIC ISER (enable) or ICER (disable) register array
 * by ::s_usb_irq_slot. Writing 1 to an ISER/ICER bit is the architected
 * way to enable/disable a single IRQ line (Arm v8-M); writing 0 is a no-op,
 * so no read-modify-write and no race with the NVIC.
 *
 * @param[in] enabled ``true`` -> ISER (enable); ``false`` -> ICER (disable).
 *
 * @pre ::s_usb_irq_slot has been resolved by ::internal_usbfs_storm_guard_init.
 * @pre ``enabled`` is a defined bool value (caller passes a literal).
 * @post The USB IRQ line is enabled / disabled at the NVIC.
 * @post No NVIC line other than ::s_usb_irq_slot is affected.
 *
 * @note ISR- and timer-safe; a single 32-bit MMIO store.
 * @since 0.1.0
 */
static void internal_usbfs_irq_set_enabled(bool enabled)
{
  const uint32_t  word = (uint32_t)s_usb_irq_slot / (uint32_t)k_nvic_irqs_per_reg;
  const uint32_t  bit  = 1UL << ((uint32_t)s_usb_irq_slot % (uint32_t)k_nvic_irqs_per_reg);
  const uintptr_t base = enabled ? (uintptr_t)k_nvic_iser_base : (uintptr_t)k_nvic_icer_base;
  *(volatile uint32_t*)(base + ((uintptr_t)word * (uintptr_t)k_nvic_reg_stride)) = bit;
}

/**
 * @brief Mask the USB IRQ at the NVIC to break an interrupt storm.
 *
 * @details Called from ::internal_usbfs_isr or ::internal_usbhs_isr once
 * a sustained run of event-less entries proves a storm is in progress.
 * With the line masked the storming controller cannot tail-chain the
 * CPU, so RTOS thread mode runs; the per-app 1 ms ``SysTick_Handler``
 * re-enables the line via ::ux_dcd_ra8_usb_irq_reenable. The line masked
 * is ::s_usb_irq_slot -- whichever controller this DCD instance drives.
 *
 * @pre Called from the active controller's ISR context.
 * @pre ::s_usb_irq_slot has been resolved at init.
 * @post USB IRQ line disabled at the NVIC until the next SysTick re-enable.
 * @post Pending USB events stay latched in INTSTS0 / BRDYSTS (level state).
 *
 * @note ISR-safe.
 * @since 0.1.0
 */
void internal_usbfs_irq_mask(void)
{
  internal_usbfs_irq_set_enabled(false);
}

/**
 * @brief Storm-guard recovery: zero the run counter and re-enable the USB IRQ.
 *
 * @details The recovery half of the USBFS interrupt-storm guard. Each USB-FS
 * app's ``SysTick_Handler`` calls this every 1 ms. Zeroing
 * ::s_isr_spurious_run makes that counter a per-millisecond rate gauge -- so
 * normal idle SOFR can never accumulate to the mask threshold -- and
 * re-enabling the NVIC line undoes any mask ::internal_usbfs_isr applied.
 * Re-enabling an already-enabled line is a no-op, so calling this outside a
 * storm is harmless. SysTick is used rather than a ThreadX ``TX_TIMER``
 * because it is an exception handler: it keeps running even while a storm
 * has thread mode -- and the ThreadX timer subsystem -- starved.
 *
 * @return No value; the helper is unconditional.
 * @note ``s_isr_spurious_run == 0`` and the USB IRQ line is enabled.
 *
 * @pre ::s_usb_irq_slot resolved (``ux_dcd_ra8_usb_initialize`` has run).
 * @pre Called from the per-app 1 ms SysTick handler (exception context).
 * @post ``s_isr_spurious_run == 0``.
 * @post NVIC re-routes the next USB event to the registered trampoline.
 *
 * @note Idempotent; intended to be called from the 1 ms SysTick handler.
 * @since 0.1.0
 */
void ux_dcd_ra8_usb_irq_reenable(void)
{
  s_isr_spurious_run = 0U;
  internal_usbfs_irq_set_enabled(true);
  /* Watchdog kick for stalled transfers: a stashed pipe transfer with
   * no pending controller event generates no IRQ, so the walk -- and
   * with it the IN-pipe strand recovery -- never runs. Pend the line
   * once per SysTick while any transfer is stashed; the ISR walk is
   * idempotent for pipes with nothing to do. */
  bool stashed = false;
  for (uint8_t i = 1U; i < (uint8_t)k_ux_dcd_ra8_usb_max_pipes; i++) {
    if (s_dcd.pipes[i].xfer != UX_NULL) {
      stashed = true;
    }
  }
  if (stashed) {
    const uint32_t  word = (uint32_t)s_usb_irq_slot / (uint32_t)k_nvic_irqs_per_reg;
    const uint32_t  bit  = 1UL << ((uint32_t)s_usb_irq_slot % (uint32_t)k_nvic_irqs_per_reg);
    const uintptr_t addr =
      (uintptr_t)k_nvic_ispr_base + ((uintptr_t)word * (uintptr_t)k_nvic_reg_stride);
    *(volatile uint32_t*)addr = bit;
  }
}

/**
 * @var s_diag
 * @brief Bridge diagnostic counter block. Read via JLink memory.
 * @note Single-writer per counter; safe under the bridge's single-
 *       worker-thread + single-class-thread model.
 * @since 0.1.0
 */
ra8_usb_dcd_diag_t s_diag = {};

/**
 * @enum ra8_usb_dcd_ctrl_id_t
 * @brief Private USBX controller-type id reported by this DCD bridge.
 */
typedef enum : uint8_t {
  k_ra8_usb_dcd_controller_id = 99U, /**< RA-USB private controller id. */
} ra8_usb_dcd_ctrl_id_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle */
/* -------------------------------------------------------------------------- */

/**
 * @brief Resolve the USB controller's NVIC slot for the storm guard.
 *
 * @details Looks up the NVIC slot the controller's ELC event was routed to
 * and records it in ::s_usb_irq_slot, so ::internal_usbfs_irq_set_enabled
 * (the mask / re-enable) targets the right ISER/ICER bit. No timer is created
 * here -- the storm guard's recovery clock is the per-app 1 ms
 * ``SysTick_Handler``, which calls ::ux_dcd_ra8_usb_irq_reenable.
 *
 * @param[in] speed Which controller was just registered.
 *
 * @pre ``ra8_isr_register`` for ``speed`` has succeeded.
 * @pre ``speed`` selects a controller present on this MCU.
 * @post ::s_usb_irq_slot holds the controller's NVIC slot (0 if lookup fails).
 * @post No NVIC enable/disable state is changed (slot resolution only).
 *
 * @note Not thread-safe; init-time only.
 * @since 0.1.0
 */
static void internal_usbfs_storm_guard_init(ra8_usb_speed_t speed)
{
  uint16_t slot = 0U;
  if (ra8_isr_lookup_slot(internal_pick_event(speed), &slot) == k_ra8_ok) {
    s_usb_irq_slot = slot;
  }
}

/**
 * @brief Bind ``_ux_system_slave->ux_system_slave_dcd`` to this bridge.
 *
 * @details Wires the USBX DCD ownership block to the bridge's dispatcher
 * trampoline, resets the per-pipe transfer stash, registers the
 * controller's ELC event onto an NVIC line via ``ra8_isr_register``, then
 * arms the FS interrupt-storm guard.
 *
 * @param[in] speed Which controller (FS or HS).
 *
 * @return ``k_ra8_ok`` on success, otherwise the failing ``ra8_err_t``.
 * @retval k_ra8_ok Bridge attached and IRQ line armed.
 * @retval k_ra8_err_invalid_state ``_ux_system_slave`` not bound.
 *
 * @pre ``ra8_usb_device_init`` and ``ra8_usb_attach_handler`` have succeeded.
 * @pre Caller is in single-threaded init context.
 * @post ``_ux_system_slave->ux_system_slave_dcd`` references this bridge.
 * @post NVIC line for the controller is armed and pointed at the trampoline.
 *
 * @note Not thread-safe; init-time only.
 * @since 0.1.0
 */
static ra8_err_t internal_init_bind_owner(ra8_usb_speed_t speed)
{
  if (_ux_system_slave == UX_NULL) {
    return k_ra8_err_invalid_state;
  }
  UX_SLAVE_DCD* owner                 = &_ux_system_slave->ux_system_slave_dcd;
  owner->ux_slave_dcd_status          = UX_DCD_STATUS_OPERATIONAL;
  owner->ux_slave_dcd_controller_type = (UINT)k_ra8_usb_dcd_controller_id; /* RA-USB private id. */
  owner->ux_slave_dcd_function        = _ux_dcd_ra8_usb_function;
  owner->ux_slave_dcd_controller_hardware = (void*)&s_dcd;

  s_dcd.speed = speed;
  s_dcd.owner = owner;
  s_dcd.state = k_ux_dcd_ra8_usb_state_ready;

  for (uint8_t i = 0U; i < (uint8_t)k_ux_dcd_ra8_usb_max_pipes; i++) {
    s_dcd.pipes[i].xfer = nullptr;
  }

#ifndef RA8_USB_POLLED_ONLY
  /* Wire the controller's ELC event onto an NVIC line. ra8_isr_init is
   * idempotent. HUM Ch 13 NVIC + Ch 14 ICU IELSR. */
  RA8_RETURN_ON_ERROR(ra8_isr_init(), s_tag, "ra8_isr_init");
  RA8_RETURN_ON_ERROR(ra8_isr_register(internal_pick_event(speed),
                                       internal_pick_isr(speed),
                                       nullptr,
                                       (uint8_t)k_ra8_usb_dcd_isr_prio,
                                       nullptr),
                      s_tag,
                      "ra8_isr_register");
  internal_usbfs_storm_guard_init(speed);
#else
  /* RA8_USB_POLLED_ONLY (TrustZone NS image, #96): the worker drives the
   * controller by calling ra8_usb_dispatch() in a tight loop instead of taking
   * the USB NVIC line. The ICU IELSR + NVIC are Secure-attributed and would
   * fault from Non-secure state, so skip ra8_isr_register entirely. Bus events,
   * chapter-9 SETUP handling, and bulk auto-echo all run inside the polled
   * dispatch -> internal_event_cb -> ux_dcd_ra8_usb_irq path. */
  (void)speed;
#endif
  return k_ra8_ok;
}

/**
 * @brief Select the active device-framework pair and parse the descriptor.
 *
 * @details Mirrors the work upstream DCDs do in their
 * ``initialize_complete`` hook (see ux_dcd_sim_slave_initialize_complete.c).
 * Picks the HS or FS device-framework slot based on
 * ``ux_system_slave_speed``, then parses the device descriptor so EP0's
 * MaxPacketSize0 is available below.
 *
 * @param[in,out] device USBX peripheral device to populate (USBX vendor
 *                       type ``UX_SLAVE_DEVICE`` -- not renamed; legacy
 *                       external API symbol).
 *
 * @pre ``_ux_system_slave`` is non-null with framework pointers set.
 * @pre ``device`` is the bound peripheral device.
 * @post Active framework pointers populated.
 * @post ``device->ux_slave_device_descriptor`` parsed when framework non-null.
 *
 * @note Not thread-safe; init-time only.
 * @since 0.1.0
 */
static void internal_init_parse_framework(UX_SLAVE_DEVICE* device)
{
  if (_ux_system_slave->ux_system_slave_speed == UX_HIGH_SPEED_DEVICE) {
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_high_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_high_speed;
  } else {
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_full_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_full_speed;
  }

  if (_ux_system_slave->ux_system_slave_device_framework != UX_NULL) {
    _ux_utility_descriptor_parse(_ux_system_slave->ux_system_slave_device_framework,
                                 _ux_system_device_descriptor_structure,
                                 UX_DEVICE_DESCRIPTOR_ENTRIES,
                                 (UCHAR*)&device->ux_slave_device_descriptor);
  }
}

/**
 * @brief Bind the EP0 transfer-request endpoint and run CREATE_ENDPOINT.
 *
 * @details Populates the EP0 transfer-request with timeout / buffer /
 * MaxPacketSize0, calls the bridge dispatcher's CREATE_ENDPOINT path so
 * EP0 ends up in the per-pipe stash, then stamps the device into the
 * ATTACHED state so the chapter-9 dispatcher accepts the host's first
 * SETUP (gate at ``_ux_device_stack_transfer_request``).
 *
 * @param[in,out] device USBX peripheral device (USBX vendor type
 *                       ``UX_SLAVE_DEVICE`` -- not renamed; legacy
 *                       external API symbol).
 * @param[in] owner DCD ownership block returned from internal_init_bind_owner.
 *
 * @pre ``device`` has a parsed device descriptor.
 * @pre ``owner`` is non-null.
 * @post EP0 transfer request fully populated.
 * @post ``device->ux_slave_device_state == UX_DEVICE_ATTACHED``.
 *
 * @note Not thread-safe; init-time only.
 * @since 0.1.0
 */
static void internal_init_setup_ep0(UX_SLAVE_DEVICE* device, UX_SLAVE_DCD* owner)
{
  UX_SLAVE_TRANSFER* tr =
    &device->ux_slave_device_control_endpoint.ux_slave_endpoint_transfer_request;
  tr->ux_slave_transfer_request_timeout              = UX_MS_TO_TICK(UX_CONTROL_TRANSFER_TIMEOUT);
  tr->ux_slave_transfer_request_current_data_pointer = tr->ux_slave_transfer_request_data_pointer;
  tr->ux_slave_transfer_request_endpoint             = &device->ux_slave_device_control_endpoint;
  device->ux_slave_device_control_endpoint.ux_slave_endpoint_descriptor.wMaxPacketSize =
    device->ux_slave_device_descriptor.bMaxPacketSize0;
  tr->ux_slave_transfer_request_requested_length =
    device->ux_slave_device_descriptor.bMaxPacketSize0;
  tr->ux_slave_transfer_request_transfer_length =
    device->ux_slave_device_descriptor.bMaxPacketSize0;

  /* Hand EP0 to ourselves so any future TRANSFER_REQUEST has the
   * pipe table populated. */
  (void)_ux_dcd_ra8_usb_function(owner,
                                 UX_DCD_CREATE_ENDPOINT,
                                 (void*)&device->ux_slave_device_control_endpoint);

  device->ux_slave_device_control_endpoint.ux_slave_endpoint_state = UX_ENDPOINT_RESET;
  tr->ux_slave_transfer_request_phase                              = UX_TRANSFER_PHASE_DATA_IN;

  /* Stamp ATTACHED so the chapter-9 dispatcher accepts the host's
   * first SETUP (gate is state in {ATTACHED, ADDRESSED, CONFIGURED}). */
  device->ux_slave_device_state = (unsigned long)UX_DEVICE_ATTACHED;
}

/**
 * @brief Bring up the USBX DCD bridge for the selected RA8 USB controller.
 *
 * @details Initialises the underlying ``ra8_usb_*`` register layer for the
 * given speed (FS or HS), attaches the bridge's ``internal_event_cb`` so
 * the dispatcher can re-enter USBX, binds the controller into the USBX
 * DCD ownership block (``_ux_system_slave``), wires the matching ELC
 * event into the NVIC, parses the active device descriptor framework,
 * and stamps EP0 so the chapter-9 dispatcher accepts the host's first
 * SETUP token. Idempotent across calls: re-running with the same speed
 * is a no-op once ``s_dcd.state`` has reached ``ready``.
 *
 * @param[in] speed Which controller to bring up
 *                  (``k_ra8_usb_speed_fs`` or ``k_ra8_usb_speed_hs``).
 *
 * @return ``ra8_err_t`` status code.
 * @retval k_ra8_ok Bridge fully initialized and ready for enumeration.
 * @retval k_ra8_err_invalid_arg ``speed`` out of range.
 * @retval k_ra8_err_internal Underlying ``ra8_usb_*`` call rejected the
 *                           initialisation step (propagated via
 *                           ``RA8_RETURN_ON_ERROR``).
 *
 * @pre ``_ux_system_slave`` is bound by ``_ux_device_stack_initialize``.
 * @pre Caller is on the USBX device task / init context (not in IRQ).
 * @post ``s_dcd.state`` is ``k_ux_dcd_ra8_usb_state_ready`` on success.
 * @post EP0 transfer request is populated and the device is in ATTACHED state.
 *
 * @note Not thread-safe; intended to run once during USBX init.
 * @since 0.1.0
 */
ra8_err_t ux_dcd_ra8_usb_initialize(ra8_usb_speed_t speed)
{
  if ((uint8_t)speed > (uint8_t)k_ra8_usb_speed_hs) {
    return k_ra8_err_invalid_arg;
  }
  RA8_RETURN_ON_ERROR(ra8_usb_device_init(speed), s_tag, "ra8_usb_device_init");
  RA8_RETURN_ON_ERROR(ra8_usb_attach_handler(speed, internal_event_cb, nullptr),
                      s_tag,
                      "ra8_usb_attach_handler");

  RA8_RETURN_ON_ERROR(internal_init_bind_owner(speed), s_tag, "bind_owner");

  /* Tell USBX system the speed. HS-controller reports HS (512-byte bulk
   * MPS); FS-controller reports FS. */
  _ux_system_slave->ux_system_slave_speed =
    (speed == k_ra8_usb_speed_hs) ? UX_HIGH_SPEED_DEVICE : UX_FULL_SPEED_DEVICE;

  UX_SLAVE_DEVICE* device = &_ux_system_slave->ux_system_slave_device;
  internal_init_parse_framework(device);
  internal_init_setup_ep0(device, s_dcd.owner);

  /* Bisect probes: SYSCFG/LPSTS state at end of DCD init, BEFORE the
   * application calls ra8_usb_device_attach(true). HUM Ch 37.2.1 SYSCFG
   * p 2060, HUM Ch 37.2.43 LPSTS p 2111. */
  if (speed == k_ra8_usb_speed_hs) {
    s_syscfg_after_dcd_init = ra8_usb_hs()->SYSCFG;
    s_lpsts_after_dcd_init  = *ra8_usbhs_lpsts();
  }

  ra8_log_info(s_tag, "DCD bridge installed");
  return k_ra8_ok;
}

/**
 * @brief Ux dcd ra usb uninitialize.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra8_err_t ux_dcd_ra8_usb_uninitialize(void)
{
  if (s_dcd.state == k_ux_dcd_ra8_usb_state_uninit) {
    return k_ra8_err_invalid_state;
  }
  /* Matching pair to the disabled ra8_isr_register in the init path. */
  (void)ra8_usb_attach_handler(s_dcd.speed, nullptr, nullptr);
  (void)ra8_usb_device_deinit(s_dcd.speed);
  if (s_dcd.owner != nullptr) {
    s_dcd.owner->ux_slave_dcd_status   = UX_DCD_STATUS_HALTED;
    s_dcd.owner->ux_slave_dcd_function = nullptr;
  }
  s_dcd.state = k_ux_dcd_ra8_usb_state_uninit;
  s_dcd.owner = nullptr;
  return k_ra8_ok;
}

/**
 * @brief Ux dcd ra usb state.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra8_usb_dcd_state_t ux_dcd_ra8_usb_state(void)
{
  return s_dcd.state;
}
