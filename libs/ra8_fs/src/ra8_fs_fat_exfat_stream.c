/*
 * ra8_fs_fat_exfat_stream.c: the exFAT stream engine moved to
 * ra8_fs_exfat_stream_abi.zig in three slices -- the allocation half
 * (RA8FW-1118), the directory-growth half (RA8FW-1119) and the stream-write
 * half (RA8FW-1120), which completes this file. The includes keep this
 * translation unit non-empty and pull in the declarations its callers see.
 */
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_exfat_stream_internal.h"
#include "ra8_fs_fat_internal.h"
