/**
 * @file port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c
 * @brief Memory and queue half of the esp-hosted OS-abstraction vtable.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * See `port/esp-hosted/src/ra8_esp_hosted_rtos_internal.h` for the contracts.
 * This translation unit owns both ThreadX byte pools -- the transport buffer
 * pool the vendored allocators draw from, and the queue-storage pool the
 * message rings are carved out of -- so the two vtable groups that depend on
 * them live together here. It also owns the host-build ThreadX model's shared
 * state; that is what the ``RA8_ESP_HOSTED_TX_SHIM_IMPL`` define below
 * selects.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_log.h"

#ifdef RA8_OFF_TARGET
/**
 * @def RA8_ESP_HOSTED_TX_SHIM_IMPL
 * @brief Elect this translation unit as owner of the ThreadX model state.
 * @details The host-build ThreadX model keeps one shared state object. Every
 * translation unit that includes the shim header sees an ``extern``
 * declaration of it except the one that defines this macro first, which gets
 * the definition. This file is that unit because it is the shim's heaviest
 * user and is always linked whenever any part of the port is.
 * @note Read-only build configuration; host build only.
 * @warning Defining it in a second translation unit is a duplicate-symbol
 *          link error, not a silent second state.
 * @par Example:
 * @code
 * #define RA8_ESP_HOSTED_TX_SHIM_IMPL
 * #include "ra8_esp_hosted_tx_shim_internal.h"
 * @endcode
 * @since 0.1.0
 */
#define RA8_ESP_HOSTED_TX_SHIM_IMPL
#include "ra8_esp_hosted_tx_shim_internal.h"
#else
#include "tx_api.h"
#endif

#include "port_esp_hosted_host_os.h"

/**
 * @var s_tag
 * @brief Log tag for the memory and queue half of the port.
 * @details Shared by every diagnostic this translation unit emits.
 * @note Static; do not access outside this TU.
 * @warning Changing it changes log-scraping expectations on the bench.
 * @since 0.1.0
 */
static const char* s_tag = "ESPH_MEM";

/**
 * @enum ra8_esp_hosted_pool_const_t
 * @brief Numeric constants of the fixed-storage allocator and queue table.
 * @details Every literal the allocator layout depends on lives here so the
 * documented block layout and the code cannot drift apart.
 * @invariant ::k_ra8_esp_hosted_hdr_bytes is at least ``sizeof`` the header
 *            struct on every supported ABI, which a ``static_assert`` checks.
 * @invariant ::k_ra8_esp_hosted_align_max is a power of two.
 * @par Example:
 * @code
 * void* p = ra8_esp_hosted_rtos_alloc(64U, k_ra8_esp_hosted_align_none);
 * @endcode
 * @see ra8_esp_hosted_rtos_alloc
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_esp_hosted_hdr_bytes        = 16U,         /**< Per-block header footprint.        */
  k_ra8_esp_hosted_hdr_magic        = 0x52384D42U, /**< 'R8MB' sentinel in each header.    */
  k_ra8_esp_hosted_align_none       = 1U,          /**< "No extra alignment" selector.     */
  k_ra8_esp_hosted_align_max        = 64U,         /**< Strictest alignment served.        */
  k_ra8_esp_hosted_alloc_max        = 8192U,       /**< Largest single payload served.     */
  k_ra8_esp_hosted_queue_words_max  = 16U,         /**< ThreadX message-size cap, words.   */
  k_ra8_esp_hosted_queue_word_bytes = 4U,          /**< Bytes in one ThreadX message word. */
  k_ra8_esp_hosted_queue_elems_max  = 64U,         /**< Deepest ring the port will make.   */
} ra8_esp_hosted_pool_const_t;

/**
 * @struct ra8_esp_hosted_alloc_hdr_t
 * @brief Bookkeeping written immediately below every allocated payload.
 * @details Carries what ThreadX byte pools do not: the exact pointer
 * ``tx_byte_allocate`` returned, the payload size ``_h_realloc`` must
 * preserve, and a sentinel that rejects a foreign pointer.
 * @invariant ``magic`` equals ::k_ra8_esp_hosted_hdr_magic in a live block.
 * @invariant ``base`` is the pointer ThreadX must be handed back.
 * @par Example:
 * @code
 * ra8_esp_hosted_alloc_hdr_t hdr = {};
 * (void)memcpy(&hdr, (const uint8_t*)p - k_ra8_esp_hosted_hdr_bytes, sizeof(hdr));
 * @endcode
 * @see ra8_esp_hosted_rtos_alloc
 * @since 0.1.0
 */
typedef struct {
  void*    base;  /**< Exact pointer ``tx_byte_allocate`` returned. */
  uint32_t magic; /**< ::k_ra8_esp_hosted_hdr_magic while live.     */
  uint32_t size;  /**< Payload bytes the caller asked for.          */
} ra8_esp_hosted_alloc_hdr_t;

static_assert(sizeof(ra8_esp_hosted_alloc_hdr_t) <= (size_t)k_ra8_esp_hosted_hdr_bytes,
              "allocation header must fit the documented 16-byte slot");

/**
 * @struct ra8_esp_hosted_queue_slot_t
 * @brief One row of the fixed queue table.
 * @details The ThreadX control block is the first member, so a slot pointer
 * and its control-block pointer are interchangeable; the port hands the slot
 * address out as the opaque queue handle.
 * @invariant ``used`` is true exactly while ``cb`` is a live ThreadX queue.
 * @invariant ``enqueued`` tracks ``cb``'s message count without an info call.
 * @par Example:
 * @code
 * queue_handle_t q = g_h.funcs->_h_create_queue(20U, 28U);
 * @endcode
 * @see ra8_esp_hosted_rtos_bind_pool
 * @since 0.1.0
 */
typedef struct {
  TX_QUEUE cb;       /**< ThreadX queue control block; must stay first. */
  void*    storage;  /**< Message ring carved from the queue pool.      */
  uint32_t enqueued; /**< Messages currently held.                      */
  bool     used;     /**< Slot occupancy flag.                          */
} ra8_esp_hosted_queue_slot_t;

/**
 * @struct ra8_esp_hosted_pool_state_t
 * @brief Module state of the memory and queue half -- entirely static.
 * @details Holds the two byte pools, their backing arrays and the queue
 * table. Nothing here is allocated; the arrays are reserved at link time.
 * @invariant ``ready`` is true exactly while both pools are live.
 * @invariant Every ``queues[i].used`` row has a live ThreadX queue.
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(true, ra8_esp_hosted_rtos_is_ready());
 * @endcode
 * @see ra8_esp_hosted_rtos_pool_init
 * @since 0.1.0
 */
typedef struct {
  TX_BYTE_POOL                transport;  /**< Transport buffer pool.      */
  TX_BYTE_POOL                queue_pool; /**< Queue message-storage pool. */
  ra8_esp_hosted_queue_slot_t queues[k_ra8_esp_hosted_max_queues]; /**< Table. */
  bool                        ready; /**< True while both pools live. */
} ra8_esp_hosted_pool_state_t;

/**
 * @var s_pool
 * @brief Singleton state of the memory and queue half.
 * @details Zero-initialised at link time; brought up by
 * ::ra8_esp_hosted_rtos_pool_init.
 * @note Static; do not access outside this TU.
 * @warning Direct modification bypasses every ThreadX consistency check.
 * @since 0.1.0
 */
static ra8_esp_hosted_pool_state_t s_pool;

/**
 * @var s_transport_mem
 * @brief Backing storage for the transport buffer pool.
 * @details Aligned to ::k_ra8_esp_hosted_align_max so a 64-byte-aligned
 * transport buffer never needs the full worst-case padding.
 * @note Static; owned exclusively by ``s_pool.transport``.
 * @warning Never write to it except through the allocator.
 * @since 0.1.0
 */
alignas(k_ra8_esp_hosted_align_max) static uint8_t s_transport_mem[k_ra8_esp_hosted_pool_bytes];

/**
 * @var s_queue_mem
 * @brief Backing storage for the queue message-storage pool.
 * @details Aligned to ::k_ra8_esp_hosted_align_max; ThreadX only needs word
 * alignment for a message ring, so this is comfortably stricter.
 * @note Static; owned exclusively by ``s_pool.queue_pool``.
 * @warning Never write to it except through a ThreadX queue service.
 * @since 0.1.0
 */
alignas(k_ra8_esp_hosted_align_max) static uint8_t s_queue_mem[k_ra8_esp_hosted_queue_pool_bytes];

/**
 * @var s_tx_name_esph_buf
 * @brief Writable ThreadX object name for the ``esph_buf`` object.
 * @details ThreadX takes object names as ``CHAR*`` rather than
 * ``const CHAR*``, so a string literal would have to be cast and would drop a
 * qualifier the object really has. A writable array removes the cast instead
 * of hiding it.
 * @note Read by the create call only; ThreadX keeps the pointer.
 * @warning Must outlive the object it names; file-scope storage does.
 * @since 0.1.0
 */
static char s_tx_name_esph_buf[] = "esph_buf";

/**
 * @var s_tx_name_esph_q
 * @brief Writable ThreadX object name for the ``esph_q`` object.
 * @details ThreadX takes object names as ``CHAR*`` rather than
 * ``const CHAR*``, so a string literal would have to be cast and would drop a
 * qualifier the object really has. A writable array removes the cast instead
 * of hiding it.
 * @note Read by the create call only; ThreadX keeps the pointer.
 * @warning Must outlive the object it names; file-scope storage does.
 * @since 0.1.0
 */
static char s_tx_name_esph_q[] = "esph_q";

/* ----------------------------------------------------------------------- */
/* Fixed-storage allocator */
/* ----------------------------------------------------------------------- */

ra8_err_t ra8_esp_hosted_rtos_pool_init(void)
{
  if (s_pool.ready) {
    ra8_log_error(s_tag, "pool already initialised");
    return k_ra8_err_invalid_state;
  }
  (void)memset(&s_pool, 0, sizeof(s_pool));
  if (tx_byte_pool_create(&s_pool.transport,
                          s_tx_name_esph_buf,
                          s_transport_mem,
                          (ULONG)sizeof(s_transport_mem)) != TX_SUCCESS) {
    ra8_log_error(s_tag, "transport byte pool refused");
    return k_ra8_err_rtos_error;
  }
  if (tx_byte_pool_create(&s_pool.queue_pool,
                          s_tx_name_esph_q,
                          s_queue_mem,
                          (ULONG)sizeof(s_queue_mem)) != TX_SUCCESS) {
    (void)tx_byte_pool_delete(&s_pool.transport);
    (void)memset(&s_pool, 0, sizeof(s_pool));
    ra8_log_error(s_tag, "queue byte pool refused");
    return k_ra8_err_rtos_error;
  }
  s_pool.ready = true;
  return k_ra8_ok;
}

ra8_err_t ra8_esp_hosted_rtos_pool_deinit(void)
{
  if (!s_pool.ready) {
    ra8_log_error(s_tag, "pool not initialised");
    return k_ra8_err_not_initialized;
  }
  ra8_err_t worst = k_ra8_ok;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_queues; ++i) {
    if (!s_pool.queues[i].used) {
      continue;
    }
    if (tx_queue_delete(&s_pool.queues[i].cb) != TX_SUCCESS) {
      worst = k_ra8_err_rtos_error;
    }
    if (tx_byte_release(s_pool.queues[i].storage) != TX_SUCCESS) {
      worst = k_ra8_err_rtos_error;
    }
    s_pool.queues[i].used = false;
  }
  if (tx_byte_pool_delete(&s_pool.queue_pool) != TX_SUCCESS) {
    worst = k_ra8_err_rtos_error;
  }
  if (tx_byte_pool_delete(&s_pool.transport) != TX_SUCCESS) {
    worst = k_ra8_err_rtos_error;
  }
  (void)memset(&s_pool, 0, sizeof(s_pool));
  return worst;
}

void ra8_esp_hosted_rtos_pool_stats(uint32_t* out_available, uint32_t* out_fragments)
{
  ULONG available = 0U;
  ULONG fragments = 0U;
  if (s_pool.ready) {
    (void)tx_byte_pool_info_get(&s_pool.transport,
                                nullptr,
                                &available,
                                &fragments,
                                nullptr,
                                nullptr,
                                nullptr);
  }
  if (out_available != nullptr) {
    *out_available = (uint32_t)available;
  }
  if (out_fragments != nullptr) {
    *out_fragments = (uint32_t)fragments;
  }
}

void* ra8_esp_hosted_rtos_alloc(size_t size, size_t align)
{
  if (!s_pool.ready) {
    return nullptr;
  }
  if ((size == 0U) || (size > (size_t)k_ra8_esp_hosted_alloc_max)) {
    return nullptr;
  }
  if ((align == 0U) || (align > (size_t)k_ra8_esp_hosted_align_max) ||
      ((align & (align - 1U)) != 0U)) {
    return nullptr;
  }
  void*       base  = nullptr;
  const ULONG total = (ULONG)(size + (size_t)k_ra8_esp_hosted_hdr_bytes + align - 1U);
  /* Two ways to end up with no memory, and both must return null: the pool
     refused, or -- a contract break by the allocator -- it reported success
     without writing the block pointer. `tx_byte_allocate` writes its
     out-parameter only on the success path, so the second term is what makes
     that invariant locally provable; without it the header write below
     computes its destination from address 0, which the clang static analyzer
     reports as a null dereference (docs/STATIC_ANALYSIS.md).
     ONE decision rather than a second early return: this file is at its
     MISRA-C 15.5 single-exit budget, and both conditions get full MC/DC from
     `test_pool_exhaustion_reports_null`. */
  if ((tx_byte_allocate(&s_pool.transport, &base, total, TX_NO_WAIT) != TX_SUCCESS) ||
      (base == nullptr)) {
    return nullptr;
  }
  const uintptr_t            raw     = (uintptr_t)base + (uintptr_t)k_ra8_esp_hosted_hdr_bytes;
  const uintptr_t            aligned = (raw + (uintptr_t)align - 1U) & ~((uintptr_t)align - 1U);
  ra8_esp_hosted_alloc_hdr_t hdr     = {};
  hdr.base                           = base;
  hdr.magic                          = (uint32_t)k_ra8_esp_hosted_hdr_magic;
  hdr.size                           = (uint32_t)size;
  (void)memcpy((void*)(aligned - (uintptr_t)k_ra8_esp_hosted_hdr_bytes), &hdr, sizeof(hdr));
  return (void*)aligned;
}

ra8_err_t ra8_esp_hosted_rtos_release(void* ptr)
{
  RA8_CHECK_NULL_PTR(ptr, s_tag, "release of a null pointer");
  ra8_esp_hosted_alloc_hdr_t hdr = {};
  (void)memcpy(&hdr,
               (const void*)((uintptr_t)ptr - (uintptr_t)k_ra8_esp_hosted_hdr_bytes),
               sizeof(hdr));
  if (hdr.magic != (uint32_t)k_ra8_esp_hosted_hdr_magic) {
    ra8_log_error(s_tag, "release of a pointer this port never handed out");
    return k_ra8_err_invalid_arg;
  }
  if (tx_byte_release(hdr.base) != TX_SUCCESS) {
    return k_ra8_err_rtos_error;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_esp_hosted_rtos_block_size(const void* ptr, size_t* out_size)
{
  RA8_CHECK_NULL_PTR(ptr, s_tag, "block_size of a null pointer");
  RA8_CHECK_NULL_PTR(out_size, s_tag, "out_size is NULL");
  ra8_esp_hosted_alloc_hdr_t hdr = {};
  (void)memcpy(&hdr,
               (const void*)((uintptr_t)ptr - (uintptr_t)k_ra8_esp_hosted_hdr_bytes),
               sizeof(hdr));
  if (hdr.magic != (uint32_t)k_ra8_esp_hosted_hdr_magic) {
    return k_ra8_err_invalid_arg;
  }
  *out_size = (size_t)hdr.size;
  return k_ra8_ok;
}

uint32_t ra8_esp_hosted_rtos_queue_words(uint32_t item_bytes)
{
  const uint32_t word  = (uint32_t)k_ra8_esp_hosted_queue_word_bytes;
  const uint32_t words = (item_bytes + (word - 1U)) / word;
  if ((item_bytes == 0U) || (words > (uint32_t)k_ra8_esp_hosted_queue_words_max)) {
    return 0U;
  }
  return words;
}

/* ----------------------------------------------------------------------- */
/* Memory vtable slots */
/* ----------------------------------------------------------------------- */

/**
 * @brief Copy a byte range for the vendored core.
 * @details Thin ``memcpy`` wrapper; the slot exists so a port may substitute
 * a DMA copy without touching the core.
 * @param[out] dest Destination buffer of at least ``size`` bytes.
 * @param[in] src Source buffer of at least ``size`` bytes.
 * @param[in] size Byte count to copy.
 * @return The destination pointer, or null when a pointer argument was null.
 * @retval dest The copy completed.
 * @retval nullptr ``dest`` or ``src`` was null; nothing was copied.
 * @pre The buffers do not overlap.
 * @pre Both buffers cover ``size`` bytes.
 * @post ``dest`` holds a byte-for-byte copy of ``src`` on success.
 * @post No allocation is performed.
 * @note Thread-safe; touches only caller storage. Validation is by return
 *       value because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_memcpy(void* dest, const void* src, uint32_t size)
{
  if ((dest == nullptr) || (src == nullptr)) {
    return nullptr;
  }
  return memcpy(dest, src, (size_t)size);
}

/**
 * @brief Fill a byte range for the vendored core.
 * @details Thin ``memset`` wrapper kept behind the vtable for the same reason
 * as ``_h_memcpy``.
 * @param[out] buf Buffer of at least ``len`` bytes.
 * @param[in] val Byte value to write, taken modulo 256 as ``memset`` does.
 * @param[in] len Byte count to write.
 * @return The buffer pointer, or null when ``buf`` was null.
 * @retval buf The fill completed.
 * @retval nullptr ``buf`` was null; nothing was written.
 * @pre ``buf`` covers ``len`` bytes.
 * @pre The caller tolerates a null return rather than a fault.
 * @post Every byte of the range equals ``val`` on success.
 * @post No allocation is performed.
 * @note Thread-safe; touches only caller storage. Validation is by return
 *       value because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_memset(void* buf, int val, size_t len)
{
  if (buf == nullptr) {
    return nullptr;
  }
  return memset(buf, val, len);
}

/**
 * @brief Serve an unaligned allocation from the transport byte pool.
 * @details Forwards to ::ra8_esp_hosted_rtos_alloc with no alignment demand,
 * so the block costs the sixteen-byte header and nothing more.
 * @param[in] size Payload bytes required.
 * @return Pointer to the payload, or null on failure.
 * @retval nullptr The substrate is down, the size is out of contract, or the
 *         pool is exhausted.
 * @retval non-null A block of at least ``size`` bytes.
 * @pre The substrate is initialised.
 * @pre ``size`` is non-zero and within ::k_ra8_esp_hosted_alloc_max.
 * @post On success the pool reports fewer bytes available.
 * @post On failure the pool is unchanged.
 * @note Thread-safe; ThreadX serialises the pool.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_malloc(size_t size)
{
  return ra8_esp_hosted_rtos_alloc(size, (size_t)k_ra8_esp_hosted_align_none);
}

/**
 * @brief Serve a zeroed array allocation from the transport byte pool.
 * @details Multiplies the two operands with an overflow guard before calling
 * the allocator, then zeroes the payload.
 * @param[in] blk_no Element count.
 * @param[in] size Bytes per element.
 * @return Pointer to the zeroed payload, or null on failure.
 * @retval nullptr An operand was zero, the product overflowed, or the pool
 *         could not satisfy the request.
 * @retval non-null A zeroed block of ``blk_no * size`` bytes.
 * @pre The substrate is initialised.
 * @pre The product is within ::k_ra8_esp_hosted_alloc_max.
 * @post On success every payload byte is zero.
 * @post On failure nothing is allocated.
 * @note Thread-safe; ThreadX serialises the pool.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_calloc(size_t blk_no, size_t size)
{
  if ((blk_no == 0U) || (size == 0U)) {
    return nullptr;
  }
  if (blk_no > ((size_t)k_ra8_esp_hosted_alloc_max / size)) {
    return nullptr;
  }
  void* const p = ra8_esp_hosted_rtos_alloc(blk_no * size, (size_t)k_ra8_esp_hosted_align_none);
  if (p != nullptr) {
    (void)memset(p, 0, blk_no * size);
  }
  return p;
}

/**
 * @brief Return a block to the transport byte pool.
 * @details Forwards to ::ra8_esp_hosted_rtos_release. Because every block
 * carries the same header, this also correctly frees an aligned block.
 * @param[in] ptr Payload pointer previously handed out by this port.
 * @pre ``ptr`` came from this port's allocator, or is null.
 * @pre The block has not already been freed.
 * @post On success the pool reports the bytes as available again.
 * @post A null or foreign pointer leaves the pool unchanged.
 * @note Thread-safe; ThreadX serialises the pool. Validation is by log
 *       because the vtable signature returns void.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_h_free(void* ptr)
{
  (void)ra8_esp_hosted_rtos_release(ptr);
}

/**
 * @brief Resize a block, preserving its contents up to the shorter length.
 * @details ThreadX byte pools cannot grow a block in place and do not record
 * its size, so this allocates a fresh block, copies ``min(old, new)`` bytes
 * from the header-recorded size, and releases the original. A null pointer
 * behaves as an allocation and a zero size behaves as a free, matching what
 * the vendored core expects of ``realloc``.
 * @param[in] mem Existing payload pointer, or null to allocate.
 * @param[in] newsize New payload size in bytes; zero frees.
 * @return Pointer to the resized payload, or null.
 * @retval nullptr The block was freed, or the new block could not be served.
 * @retval non-null A block of ``newsize`` bytes holding the old contents.
 * @pre ``mem`` came from this port's allocator, or is null.
 * @pre The substrate is initialised.
 * @post On success the first ``min(old, new)`` bytes are preserved.
 * @post On failure the original block is still valid and still owned by the
 *       caller.
 * @note Thread-safe; ThreadX serialises the pool.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_realloc(void* mem, size_t newsize)
{
  if (mem == nullptr) {
    return ra8_esp_hosted_rtos_alloc(newsize, (size_t)k_ra8_esp_hosted_align_none);
  }
  if (newsize == 0U) {
    (void)ra8_esp_hosted_rtos_release(mem);
    return nullptr;
  }
  size_t oldsize = 0U;
  if (ra8_esp_hosted_rtos_block_size(mem, &oldsize) != k_ra8_ok) {
    return nullptr;
  }
  void* const fresh = ra8_esp_hosted_rtos_alloc(newsize, (size_t)k_ra8_esp_hosted_align_none);
  if (fresh == nullptr) {
    return nullptr;
  }
  (void)memcpy(fresh, mem, (oldsize < newsize) ? oldsize : newsize);
  (void)ra8_esp_hosted_rtos_release(mem);
  return fresh;
}

/**
 * @brief Serve an aligned allocation from the transport byte pool.
 * @details Forwards to ::ra8_esp_hosted_rtos_alloc, which over-allocates and
 * stashes the base pointer immediately below the aligned payload. The
 * vendored transport asks for ``HOSTED_MEM_ALIGNMENT_64``, which is also the
 * Cortex-M85 cache-line size, so a DMA buffer can be cleaned and invalidated
 * without disturbing a neighbouring allocation.
 * @param[in] size Payload bytes required.
 * @param[in] align Required alignment; a power of two up to 64.
 * @return Pointer to the aligned payload, or null on failure.
 * @retval nullptr An argument was out of contract or the pool is exhausted.
 * @retval non-null A block whose address is a multiple of ``align``.
 * @pre The substrate is initialised.
 * @pre ``align`` is a power of two no greater than 64.
 * @post On success the returned address is a multiple of ``align``.
 * @post Worst-case overhead is ``16 + align - 1`` bytes.
 * @note Thread-safe; ThreadX serialises the pool.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_malloc_align(size_t size, size_t align)
{
  return ra8_esp_hosted_rtos_alloc(size, align);
}

/**
 * @brief Return an aligned block to the transport byte pool.
 * @details The same operation as ``_h_free``: the uniform header means the
 * release path never has to know whether the block was aligned.
 * @param[in] ptr Aligned payload pointer previously handed out by this port.
 * @pre ``ptr`` came from ``_h_malloc_align``, or is null.
 * @pre The block has not already been freed.
 * @post On success the pool reports the padding and payload as available.
 * @post A null or foreign pointer leaves the pool unchanged.
 * @note Thread-safe; ThreadX serialises the pool. Validation is by log
 *       because the vtable signature returns void.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_h_free_align(void* ptr)
{
  (void)ra8_esp_hosted_rtos_release(ptr);
}

/* ----------------------------------------------------------------------- */
/* Queue vtable slots */
/* ----------------------------------------------------------------------- */

/**
 * @brief Resolve an opaque queue handle to its live table row.
 * @details Scans the fixed table for pointer identity rather than trusting
 * the handle, so a foreign or stale pointer is rejected instead of followed.
 * @param[in] handle Opaque handle the core is holding.
 * @return The matching live slot, or null.
 * @retval nullptr The handle is null, foreign, or names a freed slot.
 * @retval non-null The live table row for that handle.
 * @pre The substrate is initialised.
 * @pre The caller does not retain the pointer past a destroy.
 * @post No state is modified.
 * @post The returned slot, when non-null, has its in-use flag set.
 * @note Thread-safe for reads; the table is only mutated during bring-up and
 *       teardown.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_esp_hosted_queue_slot_t* internal_queue_slot(const void* handle)
{
  if ((handle == nullptr) || !s_pool.ready) {
    return nullptr;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_queues; ++i) {
    if ((handle == (const void*)&s_pool.queues[i]) && s_pool.queues[i].used) {
      return &s_pool.queues[i];
    }
  }
  return nullptr;
}

/**
 * @brief Create a fixed-capacity message queue for the vendored core.
 * @details Rounds the element size up to whole ThreadX words, carves the ring
 * out of the queue byte pool and takes the first free table row. Rejects an
 * element size larger than sixteen words, which ThreadX cannot express.
 * @param[in] qnum_elem Ring depth in messages.
 * @param[in] qitem_size Element size in bytes.
 * @return Opaque queue handle, or null on failure.
 * @retval nullptr An argument was out of contract, the table is full, or the
 *         queue pool could not supply the ring.
 * @retval non-null A handle usable with the other queue slots.
 * @pre The substrate is initialised.
 * @pre ``qitem_size`` is at most sixty-four bytes.
 * @post On success one table row is occupied and reports zero messages.
 * @post On failure no ring storage is leaked.
 * @note Not thread-safe against a concurrent create or destroy.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_create_queue(uint32_t qnum_elem, uint32_t qitem_size)
{
  const uint32_t words = ra8_esp_hosted_rtos_queue_words(qitem_size);
  if (!s_pool.ready || (words == 0U)) {
    return nullptr;
  }
  if ((qnum_elem == 0U) || (qnum_elem > (uint32_t)k_ra8_esp_hosted_queue_elems_max)) {
    return nullptr;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_queues; ++i) {
    if (s_pool.queues[i].used) {
      continue;
    }
    void*       ring  = nullptr;
    const ULONG bytes = (ULONG)qnum_elem * (ULONG)words * (ULONG)k_ra8_esp_hosted_queue_word_bytes;
    if (tx_byte_allocate(&s_pool.queue_pool, &ring, bytes, TX_NO_WAIT) != TX_SUCCESS) {
      return nullptr;
    }
    if (tx_queue_create(&s_pool.queues[i].cb, s_tx_name_esph_q, (UINT)words, ring, bytes) !=
        TX_SUCCESS) {
      (void)tx_byte_release(ring);
      return nullptr;
    }
    s_pool.queues[i].storage  = ring;
    s_pool.queues[i].enqueued = 0U;
    s_pool.queues[i].used     = true;
    return &s_pool.queues[i];
  }
  ra8_log_error(s_tag, "queue table exhausted");
  return nullptr;
}

/**
 * @brief Enqueue one message, blocking for at most ``timeout``.
 * @details Zero milliseconds is a try-send; ``HOSTED_BLOCK_MAX`` (and any
 * negative value) blocks until there is room, which is what the vendored SPI
 * driver relies on when it hands a frame to the transmit queue.
 * @param[in] queue_handle Handle from ``_h_create_queue``.
 * @param[in] item Message to copy in.
 * @param[in] timeout Wait in milliseconds; 0 = try, negative = forever.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The message was enqueued.
 * @retval RET_INVALID The handle or the item pointer was not usable.
 * @retval RET_FAIL_TIMEOUT The queue stayed full for the whole wait.
 * @retval RET_FAIL ThreadX rejected the send for another reason.
 * @pre The substrate is initialised.
 * @pre ``item`` covers the element size the queue was created with.
 * @post On success the queue's message count has risen by one.
 * @post On failure the queue is unchanged.
 * @note Thread-safe; ThreadX serialises the queue. Validation is by return
 *       code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_queue_item(void* queue_handle, void* item, int timeout)
{
  ra8_esp_hosted_queue_slot_t* const slot = internal_queue_slot(queue_handle);
  if ((slot == nullptr) || (item == nullptr)) {
    return RET_INVALID;
  }
  const UINT rc = tx_queue_send(&slot->cb, item, (ULONG)ra8_esp_hosted_rtos_ms_to_ticks(timeout));
  if (rc == TX_SUCCESS) {
    slot->enqueued++;
    return RET_OK;
  }
  return (rc == TX_QUEUE_FULL) ? RET_FAIL_TIMEOUT : RET_FAIL;
}

/**
 * @brief Dequeue one message, blocking for at most ``timeout``.
 * @details A zero timeout is a non-blocking receive returning ``RET_OK`` on
 * success and non-zero when empty -- the exact shape ``spi_drv.c`` chains
 * three of together to drain its priority queues.
 * @param[in] queue_handle Handle from ``_h_create_queue``.
 * @param[out] item Receives the message.
 * @param[in] timeout Wait in milliseconds; 0 = try, negative = forever.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK A message was copied into ``item``.
 * @retval RET_INVALID The handle or the item pointer was not usable.
 * @retval RET_FAIL_TIMEOUT The queue stayed empty for the whole wait.
 * @retval RET_FAIL ThreadX rejected the receive for another reason.
 * @pre The substrate is initialised.
 * @pre ``item`` covers the element size the queue was created with.
 * @post On success the queue's message count has fallen by one.
 * @post On failure ``item`` is untouched.
 * @note Thread-safe; ThreadX serialises the queue. Validation is by return
 *       code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_dequeue_item(void* queue_handle, void* item, int timeout)
{
  ra8_esp_hosted_queue_slot_t* const slot = internal_queue_slot(queue_handle);
  if ((slot == nullptr) || (item == nullptr)) {
    return RET_INVALID;
  }
  const UINT rc =
    tx_queue_receive(&slot->cb, item, (ULONG)ra8_esp_hosted_rtos_ms_to_ticks(timeout));
  if (rc == TX_SUCCESS) {
    if (slot->enqueued > 0U) {
      slot->enqueued--;
    }
    return RET_OK;
  }
  return (rc == TX_QUEUE_EMPTY) ? RET_FAIL_TIMEOUT : RET_FAIL;
}

/**
 * @brief Report how many messages a queue is currently holding.
 * @details Reads the port's own tally rather than calling
 * ``tx_queue_info_get``: the tally is maintained by the only two functions
 * that move messages, so it costs nothing and cannot disagree.
 * @param[in] queue_handle Handle from ``_h_create_queue``.
 * @return Message count, or a negative ``RET_*`` code.
 * @retval RET_INVALID The handle was not usable.
 * @retval 0..n The number of messages waiting.
 * @pre The substrate is initialised.
 * @pre The caller treats a negative result as an error, not a count.
 * @post No queue state is modified.
 * @post The result never exceeds the ring depth.
 * @note Thread-safe for reads; the tally is a single aligned word.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_queue_msg_waiting(void* queue_handle)
{
  const ra8_esp_hosted_queue_slot_t* const slot = internal_queue_slot(queue_handle);
  if (slot == nullptr) {
    return RET_INVALID;
  }
  return (int)slot->enqueued;
}

/**
 * @brief Discard every message a queue is holding.
 * @details Flushes the ThreadX ring and clears the port's tally so the two
 * cannot drift.
 * @param[in] queue_handle Handle from ``_h_create_queue``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The queue is empty.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX rejected the flush.
 * @pre The substrate is initialised.
 * @pre The caller accepts that queued messages are lost.
 * @post The queue reports zero messages waiting.
 * @post The ring's capacity is unchanged.
 * @note Thread-safe; ThreadX serialises the queue. Validation is by return
 *       code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_reset_queue(void* queue_handle)
{
  ra8_esp_hosted_queue_slot_t* const slot = internal_queue_slot(queue_handle);
  if (slot == nullptr) {
    return RET_INVALID;
  }
  if (tx_queue_flush(&slot->cb) != TX_SUCCESS) {
    return RET_FAIL;
  }
  slot->enqueued = 0U;
  return RET_OK;
}

/**
 * @brief Delete a queue and return its ring storage to the queue pool.
 * @details Frees the table row last, so a concurrent handle lookup either
 * sees a live queue or none at all.
 * @param[in] queue_handle Handle from ``_h_create_queue``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The queue and its storage were released.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX rejected the delete or the storage release.
 * @pre The substrate is initialised.
 * @pre No thread is blocked on the queue.
 * @post The table row is free for reuse.
 * @post The ring storage is available to the queue pool again.
 * @note Not thread-safe against a concurrent create or destroy. Validation is
 *       by return code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_destroy_queue(void* queue_handle)
{
  ra8_esp_hosted_queue_slot_t* const slot = internal_queue_slot(queue_handle);
  if (slot == nullptr) {
    return RET_INVALID;
  }
  int rc = RET_OK;
  if (tx_queue_delete(&slot->cb) != TX_SUCCESS) {
    rc = RET_FAIL;
  }
  if (tx_byte_release(slot->storage) != TX_SUCCESS) {
    rc = RET_FAIL;
  }
  slot->storage  = nullptr;
  slot->enqueued = 0U;
  slot->used     = false;
  return rc;
}

ra8_err_t ra8_esp_hosted_rtos_bind_pool(hosted_osi_funcs_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "vtable is NULL");
  out->_h_memcpy            = internal_h_memcpy;
  out->_h_memset            = internal_h_memset;
  out->_h_malloc            = internal_h_malloc;
  out->_h_calloc            = internal_h_calloc;
  out->_h_free              = internal_h_free;
  out->_h_realloc           = internal_h_realloc;
  out->_h_malloc_align      = internal_h_malloc_align;
  out->_h_free_align        = internal_h_free_align;
  out->_h_create_queue      = internal_h_create_queue;
  out->_h_queue_item        = internal_h_queue_item;
  out->_h_dequeue_item      = internal_h_dequeue_item;
  out->_h_queue_msg_waiting = internal_h_queue_msg_waiting;
  out->_h_reset_queue       = internal_h_reset_queue;
  out->_h_destroy_queue     = internal_h_destroy_queue;
  return k_ra8_ok;
}
