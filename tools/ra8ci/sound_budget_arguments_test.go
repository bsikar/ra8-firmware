// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

// soundBudgetArguments is a complete, well-formed budget invocation. Each case
// below spoils exactly one part of it, so what the refusal proves is that part
// and nothing else.
func soundBudgetArguments() []string {
	return []string{"--board-id", "ek-ra8d2", "--manifest", "examples/ek_ra8d2/hw_pending/hal_timebase_demo/hil.conf",
		"--board-model", "EK-RA8D2", "--program-family", "hal_timebase_demo", "--flash-restore-bound", "45s"}
}
