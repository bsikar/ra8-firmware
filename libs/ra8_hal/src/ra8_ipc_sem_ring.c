/**
 * @file ra8_ipc_sem_ring.c
 * @brief IPC HAL -- hardware semaphores, NMI surface, and ring buffers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Sibling translation unit of ``ra8_ipc.c`` carrying the cohesive
 * lower-level IPC surfaces that compose into cross-core synchronization:
 * the IPCSEM0..15 hardware-semaphore wrappers (test-and-set + bounded
 * spin take), the NMI surface (IPC0NMI* / IPC1NMI*), and the
 * producer/consumer ring-buffer protocol that uses an IPCSEM for mutual
 * exclusion plus the FIFO IRQ lines for cross-core notification. See
 * ``ra8_ipc.h`` for the public API surface. Each register access carries
 * a HUM Ch 3 citation -- IPC is not power-gated by MSTPCR so the driver
 * does not call ``ra8_mstp_*``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_ipc.h"
#include "ra8_ipc_regs.h"

static const char* s_tag = "IPC";

static ra8_ipc_nmi_fn_t s_ipc_nmi_callback;
static void*            s_ipc_nmi_context;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate NMI unit id and return the NMI reg pointer.
 */
RA8_INTERNAL
static volatile r_ipc_nmi_regs_t* internal_ra8_ipc_get_nmi(uint8_t unit)
{
  if ((uint16_t)unit >= (uint16_t)k_ra8_ipc_nmi_unit_count) {
    return nullptr;
  }
  return ra8_ipc_nmi(unit);
}

/**
 * @brief Test-and-set on IPCSEMn -- a 32-bit read takes the lock and
 *        returns the previous LOCK value.
 *
 * @details
 * On silicon the read itself is the take: HUM Ch 3.2.3 "IPCSEMn" p 210
 * documents "Set condition: Reading this register", and the read data
 * report the LOCK state from *before* the set. The host unit-test
 * register file is dumb RAM with no read side effects, so the host
 * test build routes the same read through the ``ra8_fake_mmio``
 * read-to-set model, which performs the read and then latches LOCK in
 * the backing RAM exactly as the silicon set condition does. A test
 * models a peer core releasing the semaphore mid-spin with
 * ``ra8_fake_mmio_set_poll_hook``.
 *
 * @param[in] sem Mapped IPCSEMn register. Must not be NULL (callers
 *                null-check the accessor result before calling).
 * @return The LOCK bit as read *before* the take side effect.
 * @retval 0 The semaphore was free -- the caller now owns it.
 * @retval k_ra8_ipc_sem_mask_lock It was already owned by another core.
 *
 * @pre ``sem`` points at a mapped IPCSEMn register.
 * @pre The caller validated ``sem_id`` against ``k_ra8_ipc_sem_count``.
 * @post LOCK is set in the register (silicon set condition / host model).
 * @post No other register state is modified.
 * @note ISR-safe: the take is a single 32-bit read on silicon.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_ISR_SAFE
static uint32_t internal_ra8_ipc_sem_read_take(volatile uint32_t* sem)
{
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- the seam performs the driver's read
   * and applies the "Set condition: Reading this register" latch that
   * dumb host RAM cannot. */
  return ra8_fake_mmio_read_to_set32(sem, k_ra8_ipc_sem_mask_lock) & k_ra8_ipc_sem_mask_lock;
#else
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- "Set condition: Reading this
   * register". Reading the register sets LOCK to 1; the read result
   * is the value the register held *before* the set. */
  return *sem & k_ra8_ipc_sem_mask_lock;
#endif
}

/**
 * @brief Write the IPCSEMn write-1-to-clear release command.
 *
 * @details
 * HUM Ch 3.2.3 "IPCSEMn" p 210: "Clear condition: Writing 1 to this
 * bit" -- silicon clears LOCK when the command lands. The host
 * unit-test register file is dumb RAM that would store the literal 1
 * (leaving the register file claiming "locked" after a release), so
 * the host test build routes the same write through the
 * ``ra8_fake_mmio`` write-1-to-clear model, which leaves LOCK cleared
 * exactly as silicon does.
 *
 * @param[in] sem Mapped IPCSEMn register. Must not be NULL (callers
 *                null-check the accessor result before calling).
 *
 * @pre ``sem`` points at a mapped IPCSEMn register.
 * @pre The caller owns the semaphore (its last read-take returned 0).
 * @post LOCK reads back 0 (released) on silicon and on the host model.
 * @post No other register state is modified.
 * @note ISR-safe: the release is a single 32-bit write on silicon.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_ISR_SAFE
static void internal_ra8_ipc_sem_release_write(volatile uint32_t* sem)
{
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- the seam applies the driver's W1C
   * release command to the RAM register file the way the silicon clear
   * condition does. */
  ra8_fake_mmio_write1_clear32(sem, k_ra8_ipc_sem_mask_lock, k_ra8_ipc_sem_mask_lock);
#else
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- "Clear condition: Writing 1 to
   * this bit". */
  *sem = k_ra8_ipc_sem_mask_lock;
#endif
}

/* =============================================================================
 * Hardware semaphores
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_ipc_sem_try_take(uint8_t sem_id)
{
  if ((uint16_t)sem_id >= (uint16_t)k_ra8_ipc_sem_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile uint32_t* sem = ra8_ipc_sem(sem_id);
  RA8_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- 32-bit read sets LOCK; the read
   * value reports the *previous* state. 0 -> we just acquired,
   * 1 -> someone else already owned it. */
  const uint32_t prev = internal_ra8_ipc_sem_read_take(sem);
  if (prev != 0U) {
    return k_ra8_err_busy;
  }
  /* Acquire barrier: the critical section the caller is about to enter
   * must not be reordered ahead of taking the lock. */
  ra8_ipc_barrier();
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_sem_take_timeout(uint8_t sem_id, uint16_t max_spins)
{
  if ((uint16_t)sem_id >= (uint16_t)k_ra8_ipc_sem_count) {
    return k_ra8_err_invalid_arg;
  }
  if (max_spins == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (max_spins > k_ra8_ipc_sem_take_max) {
    max_spins = k_ra8_ipc_sem_take_max;
  }
  volatile uint32_t* sem = ra8_ipc_sem(sem_id);
  RA8_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* NASA Rule 2: bounded by ``max_spins``. */
  for (uint16_t i = 0U; i < max_spins; ++i) {
    /* HUM Ch 3.2.3 "IPCSEMn" p 210 */
    const uint32_t prev = internal_ra8_ipc_sem_read_take(sem);
    if (prev == 0U) {
      /* Acquire barrier: order the caller's critical section after the
       * successful take. */
      ra8_ipc_barrier();
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

[[nodiscard]] ra8_err_t ra8_ipc_sem_release(uint8_t sem_id)
{
  if ((uint16_t)sem_id >= (uint16_t)k_ra8_ipc_sem_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile uint32_t* sem = ra8_ipc_sem(sem_id);
  RA8_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* Release barrier: every store the caller made inside the critical
   * section must be visible to the peer before the lock is dropped. */
  ra8_ipc_barrier();
  internal_ra8_ipc_sem_release_write(sem);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_sem_is_locked(uint8_t sem_id, bool* out_locked)
{
  RA8_CHECK_NULL_PTR(out_locked, s_tag, "out_locked must not be nullptr");
  if ((uint16_t)sem_id >= (uint16_t)k_ra8_ipc_sem_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile uint32_t* sem = ra8_ipc_sem(sem_id);
  RA8_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- the 32-bit read takes the lock
   * as a side-effect, so we sample then restore the previous state. */
  const uint32_t prev = internal_ra8_ipc_sem_read_take(sem);
  *out_locked         = (prev != 0U);
  if (prev == 0U) {
    /* The caller observed "unlocked", but the diagnostic read itself
     * took the lock (HUM Ch 3.2.3 set condition). Undo the probe's own
     * take with the write-1-to-clear release command so the register
     * reads back 0 and the side effect stays invisible. */
    internal_ra8_ipc_sem_release_write(sem);
  }
  return k_ra8_ok;
}

/* =============================================================================
 * NMI surface
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_ipc_nmi_send(uint8_t unit)
{
  volatile r_ipc_nmi_regs_t* nmi = internal_ra8_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 3.2.5 "IPC0NMISET" p 211 / Ch 3.2.8 "IPC1NMISET" p 213 --
   * write 1 to SET to assert NMI on the peer. */
  nmi->NMISET = k_ra8_ipc_nmi_mask_bit;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_nmi_clear(uint8_t unit)
{
  volatile r_ipc_nmi_regs_t* nmi = internal_ra8_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 3.2.6 "IPC0NMICLR" p 212 / Ch 3.2.9 "IPC1NMICLR" p 213 --
   * write 1 to CLR to drop NMISTA.NMI. */
  nmi->NMICLR = k_ra8_ipc_nmi_mask_bit;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_nmi_get_status(uint8_t unit, bool* out_pending)
{
  RA8_CHECK_NULL_PTR(out_pending, s_tag, "out_pending must not be nullptr");
  volatile r_ipc_nmi_regs_t* nmi = internal_ra8_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 3.2.4 "IPC0NMISTA.NMI" p 210 */
  *out_pending = ((nmi->NMISTA & k_ra8_ipc_nmi_mask_bit) != 0U);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_attach_nmi_handler(ra8_ipc_nmi_fn_t fn, void* ctx)
{
  s_ipc_nmi_callback = fn;
  s_ipc_nmi_context  = ctx;
  return k_ra8_ok;
}

void ra8_ipc_dispatch_nmi(uint8_t unit)
{
  volatile r_ipc_nmi_regs_t* nmi = internal_ra8_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return;
  }
  /* HUM Ch 3.2.4 "IPC0NMISTA.NMI" p 210 -- snapshot before invoking
   * the callback so a re-entrant SET on the peer side does not
   * confuse our acknowledge. */
  if ((nmi->NMISTA & k_ra8_ipc_nmi_mask_bit) == 0U) {
    return;
  }
  const ra8_ipc_nmi_fn_t fn  = s_ipc_nmi_callback;
  void* const            ctx = s_ipc_nmi_context;
  if (fn != nullptr) {
    fn(ctx, unit);
  }
  /* HUM Ch 3.2.6 "IPC0NMICLR.CLR" p 212 -- ack so the ICU drops the
   * line. */
  nmi->NMICLR = k_ra8_ipc_nmi_mask_bit;
}

/* =============================================================================
 * Shared-memory ring-buffer protocol primitives
 * =============================================================================
 */

/**
 * @brief Validate the scalar fields of an ``ra8_ipc_ring_t`` descriptor.
 *
 * @param[in] ring Caller-supplied, null-checked ring descriptor.
 * @return ``k_ra8_ok`` if every field is in range, else
 *         ``k_ra8_err_invalid_arg``.
 *
 * @pre ring != nullptr.
 * @pre ring->slots / head / tail are non-null.
 * @post No side effects.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_ra8_ipc_ring_validate(const ra8_ipc_ring_t* ring)
{
  if (ring->capacity == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((ring->capacity & (ring->capacity - 1U)) != 0U) {
    /* Power-of-two capacity keeps the modulo cheap and unambiguous. */
    return k_ra8_err_invalid_arg;
  }
  if ((uint16_t)ring->channel >= (uint16_t)k_ra8_ipc_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint16_t)ring->sem_id >= (uint16_t)k_ra8_ipc_sem_count) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint16_t)ring->notify_id >= (uint16_t)k_ra8_ipc_irq_event_count) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_ring_init(ra8_ipc_ring_t* ring)
{
  RA8_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA8_CHECK_NULL_PTR(ring->slots, s_tag, "ring slots must not be nullptr");
  RA8_CHECK_NULL_PTR(ring->head, s_tag, "ring head must not be nullptr");
  RA8_CHECK_NULL_PTR(ring->tail, s_tag, "ring tail must not be nullptr");
  const ra8_err_t err = internal_ra8_ipc_ring_validate(ring);
  if (err != k_ra8_ok) {
    return err;
  }
  *ring->head = 0U;
  *ring->tail = 0U;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_ring_produce(ra8_ipc_ring_t* ring, uint32_t payload)
{
  RA8_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  ra8_err_t err = ra8_ipc_sem_try_take(ring->sem_id);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t head = *ring->head;
  const uint32_t tail = *ring->tail;
  if ((head - tail) >= ring->capacity) {
    /* Ring full -- release the semaphore before reporting busy so a
     * later consumer can acquire and drain. */
    (void)ra8_ipc_sem_release(ring->sem_id);
    return k_ra8_err_busy;
  }
  ring->slots[head & (ring->capacity - 1U)] = payload;
  *ring->head                               = head + 1U;
  err                                       = ra8_ipc_sem_release(ring->sem_id);
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- ring->sem_id already validated */
  }
  /* Wake the consumer through the configured IRQ event line. */
  return ra8_ipc_send_event(ring->channel, ring->notify_id);
}

[[nodiscard]] ra8_err_t ra8_ipc_ring_consume(ra8_ipc_ring_t* ring, uint32_t* out_payload)
{
  RA8_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA8_CHECK_NULL_PTR(out_payload, s_tag, "out_payload must not be nullptr");
  ra8_err_t err = ra8_ipc_sem_try_take(ring->sem_id);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t head = *ring->head;
  const uint32_t tail = *ring->tail;
  if (head == tail) {
    (void)ra8_ipc_sem_release(ring->sem_id);
    return k_ra8_err_no_data;
  }
  *out_payload = ring->slots[tail & (ring->capacity - 1U)];
  *ring->tail  = tail + 1U;
  return ra8_ipc_sem_release(ring->sem_id);
}

[[nodiscard]] ra8_err_t ra8_ipc_ring_is_empty(const ra8_ipc_ring_t* ring, bool* out_empty)
{
  RA8_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA8_CHECK_NULL_PTR(out_empty, s_tag, "out_empty must not be nullptr");
  *out_empty = (*ring->head == *ring->tail);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_ipc_ring_is_full(const ra8_ipc_ring_t* ring, bool* out_full)
{
  RA8_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA8_CHECK_NULL_PTR(out_full, s_tag, "out_full must not be nullptr");
  *out_full = ((*ring->head - *ring->tail) >= ring->capacity);
  return k_ra8_ok;
}
