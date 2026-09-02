# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show app filter help menu
default:
    @echo "APP FILTERS - Pre-configured Firmware Queues"
    @echo ""
    @echo "VALIDATED APP CATEGORIES:"
    @echo "  just apps::filter::hil                       Automated HIL hardware tests"
    @echo "  just apps::filter::manual                    Interactive board demos & LCD screens"
    @echo "  just apps::filter::c6                        ESP32-C6 wireless co-processor demos"
    @echo "  just apps::filter::stand_alone               Standalone E-Reader product firmware"
    @echo "  just apps::filter::ra8p1                     RA8P1 Cortex-M85 + NPU foundation apps"
    @echo ""
    @echo "PENDING / MAINTENANCE QUEUES:"
    @echo "  just apps::filter::pending                   Hardware validation pending queue"
    @echo "  just apps::filter::pending_c6                ESP32-C6 bring-up pending queue"
    @echo "  just apps::filter::pending_manual            Manual validation pending queue"
    @echo "  just apps::filter::revalidation              HIL revalidation queue"
    @echo "  just apps::filter::unsupported               Unsupported legacy apps"
    @echo ""

# Filter HIL automated test applications
hil:
    python3 scripts/dev/ra8_apps.py list ek_ra8d2/hw_validated/hil

# Filter manual interactive board demos
manual:
    python3 scripts/dev/ra8_apps.py list ek_ra8d2/hw_validated/manual

# Filter ESP32-C6 wireless co-processor applications
c6:
    python3 scripts/dev/ra8_apps.py list ek_ra8d2/hw_validated/c6

# Filter Standalone E-Reader product firmware
stand_alone:
    python3 scripts/dev/ra8_apps.py list board/stand_alone

# Filter RA8P1 Cortex-M85 + NPU foundation applications
ra8p1:
    python3 scripts/dev/ra8_apps.py list ra8p1_foundation

# Filter hardware validation pending applications
pending:
    python3 scripts/dev/ra8_apps.py list ek_ra8d2/hw_pending

# Filter ESP32-C6 bring-up pending applications
pending_c6:
    python3 scripts/dev/ra8_apps.py list ek_ra8d2/hw_pending/c6

# Filter manual validation pending applications
pending_manual:
    python3 scripts/dev/ra8_apps.py list ek_ra8d2/hw_pending/manual

# Filter HIL revalidation queue applications
revalidation:
    python3 scripts/dev/ra8_apps.py list ek_ra8d2/hil_needs_revalidation

# Filter unsupported legacy applications
unsupported:
    python3 scripts/dev/ra8_apps.py list _unsupported
