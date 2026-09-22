# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set dotenv-load := false

# Dry-run only the fixed private HIL compiler-cache repair on dev
check:
    /bin/bash -p "{{ justfile_directory() }}/../scripts/dev/hil_cache_repair.sh" check

# Apply only the fixed private HIL compiler-cache repair on dev
[confirm("Create or repair only ra8-hil's private compiler cache on dev?")]
apply:
    /bin/bash -p "{{ justfile_directory() }}/../scripts/dev/hil_cache_repair.sh" apply
