/**
 * @file mdl_app.c
 * @brief The application layer's one composition seam: the bound working set.
 * @details Holds the pointer a form hands to ::mdl_app_bind and nothing else.
 *          The modes reach their shared bounded state through the accessor
 *          here, so the downloader owns no storage of its own and a test can
 *          bind a fixture context in place of the production one.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_app.h"

#include "mdl_app_internal.h"

/**
 * @var s_app_ctx
 * @brief The one context every application mode currently reads.
 * @details Null until a form calls ::mdl_app_bind. It is a POINTER rather than
 *          the object because the working set is several megabytes and the
 *          form -- host static storage, or a reserved SRAM region on the
 *          device -- is the only layer that can decide where those bytes live.
 * @note Written only by ::mdl_app_bind; read only by ::priv_mdl_app_context.
 * @warning Do not name it from another translation unit; use the accessor.
 * @since 0.1.0
 */
static mdl_app_context_t* s_app_ctx = nullptr;

void mdl_app_bind(mdl_app_context_t* ctx)
{
  s_app_ctx = ctx;
}

RA8_PRIV mdl_app_context_t* priv_mdl_app_context(void)
{
  return s_app_ctx;
}
