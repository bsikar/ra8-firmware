/**
 * @file mdl_verify.h
 * @brief Bounded, no-heap structural validation of media_dl artifacts.
 */
#pragma once

#include <stddef.h>

#include "mdl_export.h"
#include "ra8_err.h"

typedef struct {
  mdl_format_t format;
  size_t       page_count;
  size_t       member_count;
  bool         metadata_present;
} mdl_verify_report_t;

/** Infer an artifact format from its complete path suffix, including multi-dot CBT wrappers. */
ra8_err_t mdl_format_from_path(const char* path, mdl_format_t* out_format);

/** True only for formats with an in-process structural validator. */
bool mdl_format_is_verifiable(mdl_format_t format);

/**
 * Validate a completed artifact using caller-owned scratch only.
 * The workspace is reset at entry and high_water reports this validation call.
 */
ra8_err_t mdl_verify_file(mdl_format_t            fmt,
                          const char*             path,
                          mdl_export_workspace_t* ws,
                          mdl_verify_report_t*    report);
