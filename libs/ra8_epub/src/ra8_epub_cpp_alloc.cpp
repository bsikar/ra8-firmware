/**
 * @file ra8_epub_cpp_alloc.cpp
 * @brief Arena-backed global `operator new` / `operator delete` for firmware.
 *
 * @details
 * The only C++ in `libs/ra8_epub` is the tinyxml2-backed XML shim
 * (`ra8_epub_xml_shim.cpp`). tinyxml2 allocates its node pools
 * (`MemPoolT<>::Alloc`), growable arrays (`DynArray::EnsureCapacity`), and
 * string storage (`StrPair::SetStr`) through the global `operator new` /
 * `operator new[]`. On the bare-metal target the default `operator new` calls
 * `malloc`, which reaches the zero-heap `_sbrk` trap (NASA Rule 3) and faults.
 *
 * This translation unit replaces the global allocation operators with thin
 * wrappers over the same static first-fit arena miniz already uses
 * (::ra8_epub_miniz_alloc / ::ra8_epub_miniz_free). It is compiled into the
 * firmware only: under `RA8_OFF_TARGET` (the host unit-test build) the body
 * is empty, so the host keeps the standard library's `malloc`-backed operators.
 *
 * The override is a documented NASA Rule 3 deviation of the same class as the
 * miniz allocator: a bounded static pool, no heap, allocations confined to a
 * single `ra8_epub_*` call. Only apps that link `ra8_epub` pull this TU in (the
 * build wires the ra8_epub C++ sources solely for them), so non-EPUB firmware
 * is unaffected.
 *
 * Exhaustion behaviour: the target is built `-fno-exceptions`, so `operator new`
 * returns `nullptr` on pool exhaustion rather than throwing `std::bad_alloc`.
 * tinyxml2 (the sole consumer) does NOT null-check its `new[]` results, so on
 * exhaustion a `memcpy` into the null block faults rather than surfacing an
 * error. In practice the pool cannot be exhausted by a conformant book: the
 * only XML tinyxml2 parses is `META-INF/container.xml` and the OPF, and the OPF
 * scratch is capped at `k_ra8_epub_opf_xml_buf`, so tinyxml2's node footprint is
 * bounded well under the arena size. This residual fault-on-exhaustion is
 * recorded as a tinyxml2 SOUP limitation in `docs/SOUP/tinyxml2.md`. The nothrow
 * `operator new` forms are also provided so any future `new(std::nothrow)` site
 * is covered.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include <cstddef>

#ifndef RA8_OFF_TARGET

#include <new>

#include "ra8_epub_miniz_alloc.h"

namespace {

/** @brief Number of element copies a single `operator new` request spans. */
constexpr std::size_t k_ra8_epub_new_one = 1U;

/**
 * @brief Allocate @p n bytes from the shared ra8_epub static arena.
 *
 * @details Routes a C++ `operator new` request through the same first-fit
 * free-list pool miniz uses. A zero-byte request is bumped to one byte so a
 * unique, freeable pointer is always returned for `new T[0]` / empty objects.
 *
 * @param[in] n Requested size in bytes.
 * @return Pointer to at least @p n bytes (max_align_t-aligned), or nullptr if
 *         the pool is exhausted.
 * @pre The arena is statically initialised on first use (idempotent).
 * @post On success the returned block is owned by the caller until freed.
 * @note Not thread-safe; the EPUB parser is single-threaded init-context.
 * @since 0.1.0
 */
void* ra8_epub_cpp_new(std::size_t n)
{
  const std::size_t want = (n != 0U) ? n : (std::size_t)1U;
  return ra8_epub_miniz_alloc(nullptr, k_ra8_epub_new_one, want);
}

} // namespace

/**
 * @brief Replacement global `operator new` (scalar).
 * @param[in] size Bytes requested by the new-expression.
 * @return Allocated, suitably aligned storage, or nullptr on exhaustion.
 */
void* operator new(std::size_t size)
{
  return ra8_epub_cpp_new(size);
}

/**
 * @brief Replacement global `operator new[]` (array).
 * @param[in] size Bytes requested by the array new-expression.
 * @return Allocated, suitably aligned storage, or nullptr on exhaustion.
 */
void* operator new[](std::size_t size)
{
  return ra8_epub_cpp_new(size);
}

/**
 * @brief Replacement global nothrow `operator new` (scalar).
 * @param[in] size Bytes requested by the new-expression.
 * @return Allocated storage, or nullptr on exhaustion (never throws).
 */
void* operator new(std::size_t size, std::nothrow_t const&) noexcept
{
  return ra8_epub_cpp_new(size);
}

/**
 * @brief Replacement global nothrow `operator new[]` (array).
 * @param[in] size Bytes requested by the array new-expression.
 * @return Allocated storage, or nullptr on exhaustion (never throws).
 */
void* operator new[](std::size_t size, std::nothrow_t const&) noexcept
{
  return ra8_epub_cpp_new(size);
}

/**
 * @brief Replacement global `operator delete` (scalar, unsized).
 * @param[in] ptr Block from ::operator_new, or nullptr.
 */
void operator delete(void* ptr) noexcept
{
  ra8_epub_miniz_free(nullptr, ptr);
}

/**
 * @brief Replacement global `operator delete[]` (array, unsized).
 * @param[in] ptr Block from ::operator_new, or nullptr.
 */
void operator delete[](void* ptr) noexcept
{
  ra8_epub_miniz_free(nullptr, ptr);
}

/**
 * @brief Replacement global `operator delete` (scalar, sized, C++14).
 * @param[in] ptr  Block from ::operator_new, or nullptr.
 * @param[in] size Original allocation size (unused; the arena tracks it).
 */
void operator delete(void* ptr, std::size_t size) noexcept
{
  (void)size;
  ra8_epub_miniz_free(nullptr, ptr);
}

/**
 * @brief Replacement global `operator delete[]` (array, sized, C++14).
 * @param[in] ptr  Block from ::operator_new, or nullptr.
 * @param[in] size Original allocation size (unused; the arena tracks it).
 */
void operator delete[](void* ptr, std::size_t size) noexcept
{
  (void)size;
  ra8_epub_miniz_free(nullptr, ptr);
}

#endif /* RA8_OFF_TARGET */
