#!/usr/bin/env sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -eu

tool_root=$1
binary=$2
source_pattern='(^|[^[:alnum:]_])(FILE|DIR|stdio|malloc|calloc|realloc|free|strdup|mmap|munmap)([^[:alnum:]_]|$)'
symbol_pattern='[[:space:]]_?(fopen|fdopen|freopen|fclose|fread|fwrite|fprintf|printf|snprintf|sprintf|fputc|fflush|fseek|ftell|tmpfile|opendir|fdopendir|readdir|closedir|scandir|malloc|calloc|realloc|free|strdup|aligned_alloc|posix_memalign|mmap|munmap)(@[^[:space:]]+)?$'

source_findings=$(find "$tool_root/inc" "$tool_root/src" -type f \( -name '*.c' -o -name '*.h' \) \
  -exec grep -En "$source_pattern" {} +)
if [ -n "$source_findings" ]; then
  printf '%s\n' "$source_findings"
  echo "forbidden hosted-stream, allocator, or mapping token in production source" >&2
  exit 1
fi

if nm -u "$binary" | grep -E "$symbol_pattern"; then
  echo "forbidden undefined production symbol" >&2
  exit 1
fi

exit 0
