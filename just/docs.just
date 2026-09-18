# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show documentation help menu
default:
    @echo "DOCS - Documentation, Dashboards & Architecture Audits"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just docs::clean          Remove generated documentation"
    @echo "  just docs::record_stats   Refresh historical HAL completion summary"
    @echo "  just docs::sizes          Summarize app .text/.data/.bss footprints"
    @echo "  just docs::audit_init     Audit peripheral init order across apps"
    @echo ""

# Remove generated documentation
clean:
    rm -rf build/docs

# Refresh the summary embedded in the historical HAL completion record
record_stats:
    python3 scripts/report/roadmap_stats.py

# Summarize built app footprints into ignored build/reports/APP_SIZES.md
sizes:
    python3 scripts/report/app_sizes.py --write

# Run the read-only init-order audit across apps
audit_init:
    python3 scripts/checks/audit_init_order.py
