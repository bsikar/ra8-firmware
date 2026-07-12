/**
 * @file ra8_compositor.h
 * @brief Zero-heap framebuffer compositor: a tiled widget tree painted into
 *        a caller-owned framebuffer through ra8_gfx, with dirty-region updates.
 *
 * @details
 * `ra8_compositor` is the *framebuffer half* of issue #145. The sibling
 * `ra8_widget` library models the pure layout / route / damage **selection**
 * logic (which widgets changed, what rectangle to flush) and deliberately
 * never touches pixels, so it can be unit-tested with mock render callbacks.
 * This library closes the remaining gap the issue asks for: it actually
 * **composites** a widget tree into an in-memory framebuffer (via `ra8_gfx`,
 * used read-only) and proves the e-ink partial-update invariant on the host --
 * compositing only the damaged region yields a buffer byte-identical to a full
 * recomposite.
 *
 * @par Widget + layout model (dwm-style tiling)
 * A scene is a **fixed pool** of widget nodes (no allocation after init,
 * NASA Rule 3). Node 0 is the root; every other node names a `parent` whose
 * index is strictly smaller (enforced by ::ra8_compositor_add), so the pool is
 * stored in parent-before-child order. There are three concrete widget kinds:
 *
 *   - **container** -- paints nothing; it only *tiles* its visible children.
 *   - **rect**      -- a solid filled rectangle (a panel / divider / chrome).
 *   - **label**     -- a background fill plus a line of ASCII text.
 *
 * Layout is a single **bounded forward sweep** with **no recursion**
 * (NASA Rule 1) and a static bound of the node count (NASA Rule 2): the root
 * is pinned to the frame, then each container -- already positioned by the
 * time the sweep reaches it -- tiles its visible children along its `axis`
 * (row or column), honouring each child's `fixed` main-axis size or `flex`
 * weight, plus the container's `pad` and `gap`. This is *stronger* than the
 * explicit-stack tree walk the issue suggests: there is no stack to bound or
 * overflow.
 *
 * @par Compositing + dirty regions
 * Painting walks the pool in the same parent-before-child order, so children
 * overdraw their parents. Every node is clipped to its own `bounds` (via
 * `ra8_gfx_set_clip`), so a node can never spill outside its rectangle. The
 * **damage** rectangle is the union of every visible *and* dirty node's bounds.
 * A dirty recomposite re-paints the whole tree clipped to that damage rectangle
 * (`ra8_gfx` discards everything outside it), which touches exactly the changed
 * pixels and leaves the rest untouched -- so it equals a full recomposite
 * within the damage and equals the previous frame outside it. That equality is
 * the property the host tests pin with a CRC golden.
 *
 * Everything is caller-owned plain data (a static node pool, no globals, no
 * allocation), so the same scene runs identically on the host test harness and
 * on the RA8D2 panel. The library depends only on `ra8_core` (errors / checks)
 * and `ra8_gfx` (the pixel backend); it modifies neither.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* ===========================================================================
 * Constants
 * ===========================================================================
 */

/**
 * @enum ra8_compositor_sentinel_t
 * @brief Sentinel values for the widget pool.
 *
 * @details
 * `k_ra8_compositor_no_parent` marks the root node, which has no parent and is
 * pinned directly to the frame by ::ra8_compositor_layout.
 */
typedef enum : uint16_t {
  k_ra8_compositor_no_parent = 0xFFFFU, /**< Root sentinel: this node has no parent. */
} ra8_compositor_sentinel_t;

/**
 * @enum ra8_compositor_kind_t
 * @brief The concrete kind of one widget node.
 *
 * @details
 * Selects how ::ra8_compositor_render paints the node: a container draws
 * nothing (it only positions its children), a rect fills its bounds with one
 * colour, and a label fills its background then draws a line of ASCII text.
 */
typedef enum : uint8_t {
  k_ra8_compositor_kind_container = 0U, /**< Layout-only group; paints nothing. */
  k_ra8_compositor_kind_rect      = 1U, /**< Solid filled rectangle.            */
  k_ra8_compositor_kind_label     = 2U, /**< Background fill plus ASCII text.   */
} ra8_compositor_kind_t;

/**
 * @enum ra8_compositor_axis_t
 * @brief Axis a container tiles its children along (dwm-style stack).
 */
typedef enum : uint8_t {
  k_ra8_compositor_axis_col = 0U, /**< Stack children top -> bottom. */
  k_ra8_compositor_axis_row = 1U, /**< Stack children left -> right. */
} ra8_compositor_axis_t;

/* ===========================================================================
 * Geometry
 * ===========================================================================
 */

/**
 * @struct ra8_compositor_rect_t
 * @brief Axis-aligned rectangle in framebuffer pixel coordinates.
 *
 * @details
 * Top-left inclusive, bottom-right exclusive: a rect with `w == 0` or
 * `h == 0` covers no pixels, and two abutting rects do not share an edge
 * pixel. All compositor geometry (frames, bounds, clips, damage) uses this
 * convention.
 *
 * @invariant `w >= 0` and `h >= 0` for any rect produced by this library.
 */
typedef struct {
  int32_t x; /**< Left edge in pixels.     */
  int32_t y; /**< Top edge in pixels.      */
  int32_t w; /**< Width in pixels (>= 0).  */
  int32_t h; /**< Height in pixels (>= 0). */
} ra8_compositor_rect_t;

/* ===========================================================================
 * Widget node + scene
 * ===========================================================================
 */

/**
 * @struct ra8_compositor_widget_t
 * @brief One widget node in the scene pool (caller-owned, no allocation).
 *
 * @details
 * A node carries its layout inputs (`parent`, `fixed` / `flex`, and the
 * container-only `axis` / `pad` / `gap`), its paint inputs (`kind`, `color`,
 * `fg`, `text`), its visibility / damage flags, and the `bounds` rectangle
 * that ::ra8_compositor_layout writes as output. Set the inputs before adding
 * the node; treat `bounds` as compositor-owned output.
 *
 * @invariant A non-root node's `parent` index is strictly less than its own.
 * @invariant `bounds.w >= 0` and `bounds.h >= 0` after layout.
 */
typedef struct ra8_compositor_widget {
  const char*           text;    /**< Label text (NUL-terminated ASCII); ignored unless label.   */
  uint32_t              color;   /**< Fill colour 0x00RRGGBB: rect fill or label background.     */
  uint32_t              fg;      /**< Label text colour 0x00RRGGBB (labels only).                */
  ra8_compositor_rect_t bounds;  /**< Resolved absolute rectangle (layout output; preset = 0).   */
  uint16_t              parent;  /**< Parent node index, or k_ra8_compositor_no_parent for root. */
  uint16_t              flex;    /**< Flex weight along the parent axis when @ref fixed == 0.    */
  int16_t               fixed;   /**< Fixed main-axis extent in px, or 0 to take a flex share.   */
  int16_t               pad;     /**< Inner padding inset in px (containers only).               */
  int16_t               gap;     /**< Gap between tiled children in px (containers only).        */
  ra8_compositor_kind_t kind;    /**< Widget kind (container / rect / label).                    */
  ra8_compositor_axis_t axis;    /**< Tiling axis for a container (row / col).                   */
  bool                  visible; /**< Laid out + painted only when true (dwm opt-in).            */
  bool                  dirty;   /**< Pending damage: re-painted on the next dirty composite.    */
} ra8_compositor_widget_t;

/**
 * @struct ra8_compositor_t
 * @brief A scene: a bound, caller-owned widget pool plus its fill level.
 *
 * @details
 * Holds the fixed `nodes` pool, its `cap`, and the number of nodes `count`
 * currently in use. ::ra8_compositor_init binds the pool; ::ra8_compositor_add
 * appends nodes in parent-before-child order. The scene allocates nothing.
 *
 * @invariant `count <= cap`.
 * @invariant `nodes` covers at least `cap` entries.
 */
typedef struct {
  ra8_compositor_widget_t* nodes; /**< Caller-owned fixed widget pool (no allocation). */
  uint16_t                 count; /**< Nodes currently in use (0..cap).                */
  uint16_t                 cap;   /**< Capacity of @ref nodes in elements.             */
} ra8_compositor_t;

/* ===========================================================================
 * Scene construction
 * ===========================================================================
 */

/**
 * @brief Bind a scene to a caller-owned, empty widget pool.
 *
 * @details
 * Records the pool pointer and capacity and resets the fill level to zero. No
 * memory is allocated; the caller owns @p pool for the scene's lifetime.
 *
 * @param[out] scene Scene to initialise (non-NULL).
 * @param[in]  pool  Backing widget pool (non-NULL; covers @p cap entries).
 * @param[in]  cap   Capacity of @p pool in elements (>= 1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Bound; the scene is empty.
 * @retval k_ra8_err_null_ptr    @p scene or @p pool is NULL.
 * @retval k_ra8_err_invalid_arg @p cap is 0.
 *
 * @pre @p scene and @p pool are non-NULL.
 * @pre @p cap is at least 1.
 * @post On success `scene->count == 0`, `scene->cap == cap`.
 * @post On failure no field of @p scene is modified.
 *
 * @note Not thread-safe.
 * @see ra8_compositor_add
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_compositor_init(ra8_compositor_t* scene, ra8_compositor_widget_t* pool, uint16_t cap);

/**
 * @brief Append a widget node to the scene under a given parent.
 *
 * @details
 * Copies @p widget into the next free pool slot, overwrites its `parent` with
 * @p parent, and returns the new node's index. The first node added must be
 * the root (`parent == k_ra8_compositor_no_parent`); every later node must name
 * an already-added parent (index `< scene->count`), which keeps the pool in
 * parent-before-child order so layout and paint need no recursion.
 *
 * @param[in,out] scene     Initialised scene (non-NULL).
 * @param[in]     widget    Node to copy in (non-NULL).
 * @param[in]     parent    Parent index, or k_ra8_compositor_no_parent for the root.
 * @param[out]    out_index Receives the new node's index (non-NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Added; `*out_index` is the new node.
 * @retval k_ra8_err_null_ptr    @p scene, @p widget, or @p out_index is NULL.
 * @retval k_ra8_err_no_mem      The pool is full (`count == cap`).
 * @retval k_ra8_err_invalid_arg Bad parent: a non-root references an index that
 *                              is not already present, or the root (first node)
 *                              does not use the no-parent sentinel.
 *
 * @pre @p scene, @p widget, @p out_index are non-NULL.
 * @pre The first node uses `k_ra8_compositor_no_parent`; later nodes use an
 *      existing parent index.
 * @post On success `scene->count` grew by one and `*out_index == count - 1`.
 * @post On failure the scene is unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_compositor_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_add(ra8_compositor_t*              scene,
                                           const ra8_compositor_widget_t* widget,
                                           uint16_t                       parent,
                                           uint16_t*                      out_index);

/* ===========================================================================
 * Layout
 * ===========================================================================
 */

/**
 * @brief Resolve every node's absolute bounds by a bounded tiling sweep.
 *
 * @details
 * Pins the root (node 0) to @p frame, then sweeps the pool once: each visible
 * container tiles its visible children inside its own content rectangle (its
 * bounds inset by `pad`) along its `axis`, separated by `gap`. A child with
 * `fixed > 0` takes that many main-axis pixels; the remaining main-axis space
 * is split among the flex children in proportion to their `flex` weight. The
 * cross-axis extent fills the container's content. Because each container is
 * positioned before the sweep reaches it (parent index < own index), one
 * forward pass suffices -- no recursion (NASA Rule 1), bounded by node count
 * (NASA Rule 2). Invisible nodes (and their subtrees) are skipped.
 *
 * @param[in,out] scene Scene whose nodes receive resolved bounds (non-NULL).
 * @param[in]     frame Outer rectangle the root fills (non-NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Laid out; every visible node has bounds.
 * @retval k_ra8_err_null_ptr    @p scene or @p frame is NULL.
 * @retval k_ra8_err_invalid_arg The scene is empty (`count == 0`).
 *
 * @pre @p scene and @p frame are non-NULL.
 * @pre @p scene has at least one node (the root).
 * @post On success the root's bounds equal @p frame.
 * @post On success every visible node's bounds lie within its parent content.
 *
 * @note Not thread-safe.
 * @see ra8_compositor_render
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_layout(ra8_compositor_t*            scene,
                                              const ra8_compositor_rect_t* frame);

/* ===========================================================================
 * Compositing
 * ===========================================================================
 */

/**
 * @brief Paint every visible node into the bound ra8_gfx framebuffer.
 *
 * @details
 * Walks the pool in parent-before-child order so children overdraw parents.
 * Each node is clipped to the intersection of its own `bounds` and @p clip
 * (NULL means the whole framebuffer), so no node draws outside its rectangle
 * and a dirty recomposite touches only the damaged pixels. Containers paint
 * nothing; rects fill their bounds; labels fill their background then draw
 * their text. The ra8_gfx clip is reset to the full framebuffer on return.
 * Pixel-level drawing is delegated to `ra8_gfx`, which the caller must have
 * bound to a framebuffer with `ra8_gfx_init`.
 *
 * @param[in] scene Laid-out scene (non-NULL).
 * @param[in] clip  Region to confine painting to, or NULL for the full frame.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Painted (clipped as requested).
 * @retval k_ra8_err_null_ptr    @p scene is NULL.
 * @retval k_ra8_err_invalid_arg The scene is empty (`count == 0`).
 *
 * @pre @p scene is non-NULL and has been laid out.
 * @pre `ra8_gfx_init` has bound a framebuffer.
 * @post Visible nodes intersecting @p clip are painted; others are untouched.
 * @post The ra8_gfx clip rectangle is the full framebuffer on return.
 *
 * @note Not thread-safe; shares the single global ra8_gfx bind + clip state.
 * @see ra8_compositor_damage
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_render(const ra8_compositor_t*      scene,
                                              const ra8_compositor_rect_t* clip);

/**
 * @brief Compute the damage rectangle: the union of visible, dirty nodes.
 *
 * @details
 * Unions the `bounds` of every node that is both visible and dirty into one
 * bounding rectangle and reports how many such nodes there were. Passing that
 * rectangle to ::ra8_compositor_render re-paints exactly the changed pixels.
 * When nothing is dirty the rectangle is empty (`w == h == 0`).
 *
 * @param[in]  scene     Laid-out scene (non-NULL).
 * @param[out] out_rect  Receives the union rectangle (non-NULL).
 * @param[out] out_count Receives the number of dirty visible nodes (non-NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Computed (see @p out_count).
 * @retval k_ra8_err_null_ptr @p scene, @p out_rect, or @p out_count is NULL.
 *
 * @pre @p scene, @p out_rect, @p out_count are non-NULL.
 * @pre @p scene has been laid out (bounds are current).
 * @post `*out_count == 0` iff nothing was dirty (then `*out_rect` is empty).
 * @post `*out_rect` encloses the bounds of every dirty visible node.
 *
 * @note Pure; not thread-safe versus concurrent mutation.
 * @see ra8_compositor_render
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_damage(const ra8_compositor_t* scene,
                                              ra8_compositor_rect_t*  out_rect,
                                              uint16_t*               out_count);

/**
 * @brief Mark one node dirty so the next dirty composite re-paints it.
 *
 * @param[in,out] scene Scene owning the node (non-NULL).
 * @param[in]     index Node index to invalidate (`< scene->count`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Node marked dirty.
 * @retval k_ra8_err_null_ptr    @p scene is NULL.
 * @retval k_ra8_err_invalid_arg @p index is out of range.
 *
 * @pre @p scene is non-NULL.
 * @pre @p index is less than `scene->count`.
 * @post On success `scene->nodes[index].dirty == true`.
 * @post On failure the scene is unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_compositor_clear_dirty
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_invalidate(ra8_compositor_t* scene, uint16_t index);

/**
 * @brief Clear the dirty flag on every node in the scene.
 *
 * @details
 * Called after a composite to acknowledge the painted damage, so the next
 * ::ra8_compositor_damage starts from a clean slate.
 *
 * @param[in,out] scene Scene to clean (non-NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           All nodes cleaned.
 * @retval k_ra8_err_null_ptr @p scene is NULL.
 *
 * @pre @p scene is non-NULL.
 * @pre @p scene was initialised (`count <= cap`).
 * @post Every node has `dirty == false`.
 * @post No other field is modified.
 *
 * @note Not thread-safe.
 * @see ra8_compositor_invalidate
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_clear_dirty(ra8_compositor_t* scene);

/**
 * @brief Lay out and fully composite the whole scene, then clear damage.
 *
 * @details
 * The full-frame compose cycle: ::ra8_compositor_layout against @p frame,
 * ::ra8_compositor_render with no clip (every visible node painted), then
 * ::ra8_compositor_clear_dirty. Use this for the first frame or any full
 * redraw (e-ink GC16-style).
 *
 * @param[in,out] scene Scene to compose (non-NULL).
 * @param[in]     frame Outer rectangle the root fills (non-NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Laid out, painted, and cleaned.
 * @retval k_ra8_err_null_ptr    @p scene or @p frame is NULL.
 * @retval k_ra8_err_invalid_arg The scene is empty (forwarded from layout).
 *
 * @pre @p scene and @p frame are non-NULL; `ra8_gfx_init` bound a framebuffer.
 * @pre @p scene has at least one node.
 * @post On success the framebuffer holds the full scene and no node is dirty.
 * @post On failure the framebuffer / scene reflect the failed step only.
 *
 * @note Not thread-safe.
 * @see ra8_compositor_compose_dirty
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_compose_full(ra8_compositor_t*            scene,
                                                    const ra8_compositor_rect_t* frame);

/**
 * @brief Lay out and composite only the damaged region, then clear damage.
 *
 * @details
 * The partial-update compose cycle the issue asks for: ::ra8_compositor_layout
 * against @p frame, ::ra8_compositor_damage to find the union of the dirty
 * nodes, ::ra8_compositor_render clipped to that damage (a no-op when nothing
 * is dirty), then ::ra8_compositor_clear_dirty. The painted result inside the
 * damage rectangle is byte-identical to a full recomposite, while pixels
 * outside it are left untouched -- the e-ink fast/A2 partial update.
 *
 * @param[in,out] scene      Scene to compose (non-NULL).
 * @param[in]     frame      Outer rectangle the root fills (non-NULL).
 * @param[out]    out_damage Receives the damage rectangle flushed (non-NULL).
 * @param[out]    out_count  Receives the number of dirty nodes painted (non-NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Composited; see @p out_count / @p out_damage.
 * @retval k_ra8_err_null_ptr    Any pointer argument is NULL.
 * @retval k_ra8_err_invalid_arg The scene is empty (forwarded from layout).
 *
 * @pre All pointer arguments are non-NULL; `ra8_gfx_init` bound a framebuffer.
 * @pre @p scene has at least one node.
 * @post On success no node is dirty and `*out_damage` is the flushed rect.
 * @post `*out_count == 0` iff nothing was dirty (then `*out_damage` is empty).
 *
 * @note Not thread-safe.
 * @see ra8_compositor_compose_full
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compositor_compose_dirty(ra8_compositor_t*            scene,
                                                     const ra8_compositor_rect_t* frame,
                                                     ra8_compositor_rect_t*       out_damage,
                                                     uint16_t*                    out_count);

#ifdef __cplusplus
}
#endif
