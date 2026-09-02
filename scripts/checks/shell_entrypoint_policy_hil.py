# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Raw typed-policy rows for the HIL shell domain.

The aggregate module converts these compact rows into ``ShellPolicy`` values.
Keeping HIL in a separate domain makes the authority reviewable without
letting the exhaustive aggregate exceed the repository file-size cap.
"""

from __future__ import annotations

from typing import TypeAlias

ShellPolicyRow: TypeAlias = tuple[str, str, str, str, bool, bool]

HIL_POLICY_ROWS: tuple[ShellPolicyRow, ...] = (
    ("scripts/hil/all.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/bench.sh", "privileged", "entry", "bash", False, False),
    ("scripts/hil/bench_contention.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/bench_hold_and_work.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/bench_unguarded_probe.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/camera_livestream.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/camera_picture.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/camera_tunnel.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/camera_video.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/check_alive.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/dev.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/dlm_reset.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/dlm_reset_local.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/erase.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/eth_tcp.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/exit_low_power.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/find_jlink.sh", "privileged", "entry", "bash", False, False),
    ("scripts/hil/flash.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/flash_retry.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/hid_test.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/jlink_memprobe.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/lib/bench_client.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/bench_host.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/bench_human.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/bench_lock.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/bench_selftest.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/hil_conf.sh", "privileged", "dual-use", "bash", False, True),
    ("scripts/hil/lib/preflash_guard.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/privileged_helper.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/python_env.sh", "privileged", "dual-use", "bash", True, True),
    ("scripts/hil/lib/rig_contract.sh", "privileged", "dual-use", "bash", True, True),
    ("scripts/hil/lib/rig_env.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/lib/tty_resolve.sh", "privileged", "sourced-only", "bash", False, True),
    ("scripts/hil/msc_test.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/ppps.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/probe.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/recover.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/reflash.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/rtt_scrape.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/run.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/run_direct.sh", "privileged", "entry", "bash", False, False),
    ("scripts/hil/run_local.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/tapo.sh", "privileged", "entry", "bash", True, False),
    ("scripts/hil/usb_test.sh", "privileged", "entry", "bash", True, False),
)
