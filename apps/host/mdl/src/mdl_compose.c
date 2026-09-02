/**
 * @file mdl_compose.c
 * @brief The composition of THIS form: libcurl in, one parsed command out.
 * @details Two translations and nothing else. The first binds the concrete
 *          libcurl backend into the abstract ::mdl_net_provider_t seam the
 *          application layer consumes, over storage this form owns. The second
 *          turns one validated ::mdl_args_t plus the mode ::mdl_cli_validate
 *          chose into a call on one ::mdl_app.h entry point. Keeping both here
 *          is what lets `main.c` stay process plumbing and the downloader stay
 *          free of a command line.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_compose_internal.h"
#include "mdl_net_curl.h"

/**
 * @var s_curl_storage
 * @brief The one caller-owned libcurl backend context this form provides.
 * @details Static rather than per-call so the provider can hand the same bytes
 *          to every run without the application layer knowing a backend needs
 *          storage at all. One object is enough because runs are strictly
 *          serial: a mode destroys its interface before any other mode opens
 *          one, and discovery destroys its own before dispatching a series.
 * @note Written only through ::internal_curl_open.
 * @warning Never reuse it while an interface opened from it is still live.
 * @since 0.1.0
 */
static mdl_net_curl_storage_t s_curl_storage;

/**
 * @brief Open one hardened libcurl interface over the form's backend storage.
 * @details The concrete half of the ::mdl_net_provider_t seam; the factory's
 *          private storage arrives as @p ctx so this function names no global.
 * @param[in,out] ctx The provider's ::mdl_net_curl_storage_t.
 * @param[in] policy Session security policy to harden the handle with.
 * @param[out] out_net Interface populated on success.
 * @return Canonical backend initialisation status.
 * @retval k_ra8_ok @p out_net owns a hardened libcurl easy handle.
 * @retval other The libcurl backend rejected the policy or failed to init.
 * @pre @p ctx addresses this form's backend storage.
 * @pre Credential bytes referenced by @p policy stay readable until destroy.
 * @post Failure leaves @p out_net and @p ctx holding only zero bytes.
 * @post Success transfers no ownership of @p ctx.
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_curl_open(void* ctx, const mdl_net_policy_t* policy, mdl_net_iface_t* out_net)
{
  return mdl_net_curl_init(out_net, (mdl_net_curl_storage_t*)ctx, policy);
}

/**
 * @var s_curl_provider
 * @brief This form's injected transport factory, bound once at compile time.
 * @details Immutable, so the seam cannot be repointed at run time by anything
 *          other than a different composition root.
 * @note Published through ::priv_mdl_compose_net_provider.
 * @warning Do not name it from another translation unit.
 * @since 0.1.0
 */
static const mdl_net_provider_t s_curl_provider = {.ctx  = &s_curl_storage,
                                                   .open = internal_curl_open};

RA8_PRIV const mdl_net_provider_t* priv_mdl_compose_net_provider(void)
{
  return &s_curl_provider;
}

RA8_PRIV mdl_series_run_t priv_mdl_compose_build_run(const mdl_args_t*     a,
                                                     mdl_format_t          format,
                                                     const mdl_run_opts_t* opts,
                                                     const mdl_nums_t*     n)
{
  return (mdl_series_run_t){.cfg_path     = a->cfg,
                            .series_url   = a->series,
                            .out_dir      = a->out,
                            .cache_dir    = a->cache_dir,
                            .format       = format,
                            .combine      = !a->separate,
                            .update       = a->update,
                            .from_present = n->from_present,
                            .from_num     = n->from_num,
                            .chapters     = n->chapters,
                            .seed         = n->seed,
                            .timeout      = n->timeout,
                            .opts         = opts};
}

RA8_PRIV int priv_mdl_compose_dispatch(const mdl_args_t*       a,
                                       mdl_cli_mode_t          mode,
                                       mdl_format_t            format,
                                       const mdl_run_opts_t*   opts,
                                       const mdl_nums_t*       nums,
                                       const mdl_series_run_t* run)
{
  switch (mode) {
    case k_mdl_cli_mode_series:
      return mdl_app_run_series(run);
    case k_mdl_cli_mode_search:
    case k_mdl_cli_mode_browse: {
      const mdl_discover_run_t request = {.cfg_path = a->cfg,
                                          .term     = a->search,
                                          .pick     = nums->pick,
                                          .timeout  = nums->timeout,
                                          .seed     = nums->seed,
                                          .browse   = a->browse};
      return mdl_app_run_discover(&request, opts, run);
    }
    case k_mdl_cli_mode_list:
      return mdl_app_run_list(a->out);
    case k_mdl_cli_mode_update_all:
      return mdl_app_run_update_all(run);
    case k_mdl_cli_mode_remove:
      return mdl_app_run_remove(a->out, a->remove_series);
    case k_mdl_cli_mode_verify:
      return mdl_app_run_verify((a->verify_dir != nullptr) ? a->verify_dir : a->out);
    case k_mdl_cli_mode_init_site:
      return mdl_app_run_init_site(a->init_site_url, a->out);
    case k_mdl_cli_mode_pack:
      return mdl_app_run_pack(a->pack, format);
    case k_mdl_cli_mode_artifact:
      return mdl_app_run_artifact(a->page_url, a->out, nums->timeout, opts);
    case k_mdl_cli_mode_page:
      return mdl_app_run_page(a->page_url,
                              a->out,
                              a->attr,
                              nums->max_imgs,
                              nums->seed,
                              nums->timeout,
                              opts);
    default:
      return 2;
  }
}
