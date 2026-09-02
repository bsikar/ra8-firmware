/**
 * @file mdl_compose_internal.h
 * @brief What this FORM injects, and how one parsed invocation reaches a mode.
 * @details The only place the argv grammar and the portable application layer
 *          meet. Everything here is a translation -- an ::mdl_args_t field into
 *          a value ::mdl_app.h already names, or a concrete host backend into
 *          the seam that consumes it -- so nothing above it has to know a
 *          command line exists and nothing below it names libcurl.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "mdl_app.h"
#include "mdl_cli.h"
#include "mdl_format.h"
#include "ra8_attributes.h"

/**
 * @brief The libcurl transport factory this form injects into every run.
 * @details Returns the one provider whose `open` wires ::mdl_net_curl_init over
 *          this form's own backend storage. A run mode receives it through
 *          ::mdl_run_opts_t and never names libcurl; another form returns a
 *          different provider from its own composition and the modes are
 *          unchanged.
 * @return The provider to publish in ::mdl_run_opts_t::net.
 * @retval non-NULL A process-lifetime provider valid for the whole run.
 * @pre The process is single-threaded with respect to network runs.
 * @pre No interface previously opened from this provider is still live.
 * @post No caller-visible state is modified.
 * @post Successive calls return the same provider.
 * @note Not thread-safe: one backend storage serves one serial run at a time.
 * @since 0.1.0
 */
RA8_PRIV const mdl_net_provider_t* priv_mdl_compose_net_provider(void);

/**
 * @brief Translate validated argv state into one portable series-run value.
 * @details Copies only validated selections, numeric bounds, output format and
 *          policy pointers, so the application layer sees values rather than
 *          the argv-shaped structs that produced them.
 * @param[in] a Parsed and validated command-line options.
 * @param[in] format Validated output format.
 * @param[in] opts Validated run policy, including the injected transport.
 * @param[in] n Validated numeric command options.
 * @return The assembled run description.
 * @retval mdl_series_run_t A value borrowing the caller-owned argument storage.
 * @pre @p a, @p opts and @p n are non-NULL and were validated.
 * @pre Referenced strings outlive every dispatched run function.
 * @post The returned value owns no newly allocated storage.
 * @post The input objects remain unchanged.
 * @note Thread-safe for independent input objects.
 * @since 0.1.0
 */
RA8_PRIV mdl_series_run_t priv_mdl_compose_build_run(const mdl_args_t*     a,
                                                     mdl_format_t          format,
                                                     const mdl_run_opts_t* opts,
                                                     const mdl_nums_t*     n);

/**
 * @brief Route the one validated mode to its portable application entry point.
 * @details The whole of the argv-to-application translation: every case reads
 *          the fields that mode needs out of @p a and @p nums and calls one
 *          ::mdl_app.h entry point. The mode enum already encodes the selection
 *          strictly, so no case re-derives it from option precedence.
 * @param[in] a Parsed and validated command-line options.
 * @param[in] mode The one mode ::mdl_cli_validate selected.
 * @param[in] format Validated output format.
 * @param[in] opts Validated run policy, including the injected transport.
 * @param[in] nums Validated numeric command options.
 * @param[in] run Prepared series-run template.
 * @return Process-style status from the selected mode.
 * @retval 0 The selected operation completed successfully.
 * @retval 1 The selected operation reported an execution failure.
 * @retval 2 The mode value was not one validation can produce.
 * @pre All pointer arguments are non-NULL.
 * @pre @p mode was produced by ::mdl_cli_validate and a context is bound.
 * @post Exactly one mode entry point is invoked.
 * @post Non-selected mode entry points perform no work.
 * @note Not thread-safe because the modes share one bound context.
 * @since 0.1.0
 */
RA8_PRIV int priv_mdl_compose_dispatch(const mdl_args_t*       a,
                                       mdl_cli_mode_t          mode,
                                       mdl_format_t            format,
                                       const mdl_run_opts_t*   opts,
                                       const mdl_nums_t*       nums,
                                       const mdl_series_run_t* run);
