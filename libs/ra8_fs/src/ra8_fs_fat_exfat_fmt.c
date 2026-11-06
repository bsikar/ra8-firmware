/*
 * ra8_fs_fat_exfat_fmt.c: the exFAT formatter moved to
 * ra8_fs_exfat_fmt_abi.zig in two slices -- the geometry and boot half
 * (RA8FW-1121), and the FAT/bitmap/root/MBR half with the
 * priv_exfat_format orchestrator (RA8FW-1122), which completes this file.
 * The includes keep this translation unit non-empty and pull in the
 * declarations its callers see.
 */
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
