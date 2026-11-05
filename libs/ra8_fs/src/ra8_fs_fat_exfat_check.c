/*
 * ra8_fs_fat_exfat_check.c: the exFAT fs-check scan moved to
 * ra8_fs_check_abi.zig (RA8FW-1117). The includes keep this translation unit
 * non-empty and pull in the declarations its callers see.
 */
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_check.h"
#include "ra8_fs_fat_check_internal.h"
#include "ra8_fs_fat_internal.h"
