// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"fmt"
	"time"
)

// An attempt reserves recovery twice over at exactly one of its steps. Every
// step runs with the caller's recoveryMargin; the observation step, the one
// that leaves the board holding a program somebody has to put back, also
// reserves the pinned decision's FlashRestoreBound on top of it
// (hil_execute.go). Nothing judged that sum. RunSegment refuses a
// recoveryMargin above maxBoardOperation and is the only door the sum ever
// reaches, so the refusal arrived in the middle of the attempt rather than
// before it.
//
// Both halves are ordinary. The caller's margin is bounded by RunHILAttempt's
// own entry check at maxBoardOperation, and the restore bound is held to the
// catalog when the assignment is claimed (boardclient.validHILTimingAssignment
// crosses it against HILTask.FlashRestoreSeconds, which catalog admits up to
// 3600). Two independently valid values are all it takes: an hour of caller
// margin beside any declared restore bound is already over the ceiling. This
// is not about a server sending nonsense.
//
// What it costs to find out late is hardware left mid-attempt. A board is
// flashed before it is watched, and catalog.ValidateTask admits a HIL task
// only when one of its steps IS the observation step, so the refusal lands
// after the earlier steps have run: the board is programmed, the step that
// would have observed it is refused as an invalid agent configuration, and
// the attempt records that against work which did reach the fixture. Every
// other refusal in this method is asked before the loop for exactly that
// reason, and both halves of this one are known just as early.

// checkReservedRecoveryIsOperable reports whether the recovery an attempt
// would reserve at its observation step is one RunSegment will accept. It
// judges the sum only; each half is already judged where it enters.
func checkReservedRecoveryIsOperable(recoveryMargin, flashRestoreBound time.Duration) error {
	if flashRestoreBound < 0 {
		return fmt.Errorf("%w: pinned HIL timing states a negative flash restore bound", ErrInvalidAgent)
	}
	if recoveryMargin+flashRestoreBound > maxBoardOperation {
		return fmt.Errorf("%w: recovery reserved at the HIL observation step exceeds the maximum board operation", ErrInvalidAgent)
	}
	return nil
}
