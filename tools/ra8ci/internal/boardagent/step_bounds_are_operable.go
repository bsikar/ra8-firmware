// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"fmt"
	"time"
)

// Every step of a HIL attempt is run as a board segment, and RunSegment
// refuses a bound above maxBoardOperation as an invalid agent configuration.
// Nothing asked that question before the loop. The two bounds an attempt can
// ask for are both set elsewhere and neither is held to this ceiling where it
// is set: an ordinary step is bounded by the task's own deadline, which
// catalog.ValidateTask admits all the way to 86400 seconds, and the
// observation step is bounded by the server-pinned decision's validity
// window, which this agent compares against the manifest and the local safety
// maximum but never against the ceiling its own segments run under.
//
// The loop clamps each bound to the time left before the attempt deadline, so
// whether an over-wide bound is actually refused depends on when the attempt
// starts: the same task runs fine claimed twenty minutes before its deadline
// and is refused claimed two hours before it. That is the worst shape to find
// out about late, because the refusal is not reproducible from the task alone.
//
// What it costs to find out late is the same thing checkReservedRecoveryIsOperable
// was written for: a board left mid-attempt. The observation step is the one
// that runs after the board has been programmed, so a validity window above
// the ceiling is refused with the fixture already holding a program, and the
// attempt records that against work which did reach the hardware.

// checkStepBoundsAreOperable reports whether the segment bounds this attempt
// will ask for are ones RunSegment will accept. It judges only what the loop
// will actually request: a bound wider than the attempt's own span is clamped
// to that span before it reaches a segment, so a long task deadline under a
// short attempt is ordinary and passes here.
func checkStepBoundsAreOperable(taskDeadline, validityWindow, attemptSpan time.Duration) error {
	if attemptSpan <= 0 {
		return fmt.Errorf("%w: attempt deadline does not follow its start", ErrInvalidAgent)
	}
	if taskDeadline <= 0 {
		return fmt.Errorf("%w: task states no positive deadline to bound a segment with", ErrInvalidAgent)
	}
	if requestedBound(taskDeadline, attemptSpan) > maxBoardOperation {
		return fmt.Errorf("%w: task deadline bounds a segment above the maximum board operation", ErrInvalidAgent)
	}
	if requestedBound(validityWindow, attemptSpan) > maxBoardOperation {
		return fmt.Errorf("%w: pinned HIL validity window bounds a segment above the maximum board operation", ErrInvalidAgent)
	}
	return nil
}

// requestedBound is the bound a step declaring this duration reaches a segment
// with, mirroring the clamp RunHILAttempt applies before every RunSegment call.
func requestedBound(declared, attemptSpan time.Duration) time.Duration {
	if declared > attemptSpan {
		return attemptSpan
	}
	return declared
}
