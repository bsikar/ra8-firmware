// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"fmt"
	"time"
)

// scanSkewTolerance is the slack this rule allows, and it is wider than
// overlapTolerance on purpose. Every other rule in this package holds a
// runner's timestamps against timestamps from the SAME runner, so five
// seconds of rounding is all the slack those comparisons need. This one
// crosses two machines: the runner that stamped the step and whatever host
// ran the scan. A CI box or a laptop a minute off its own time is ordinary
// and must not fail the gate, while the fault this looks for is the one #509
// recorded, a host minutes or hours out.
const scanSkewTolerance = 2 * time.Minute

// stampedAheadOfTheScan catches the clock fault the in-job rules cannot see:
// a runner that was ALREADY ahead when the job began.
//
// jobBeganWithinItsRun closed half of that gap by holding a step's start
// against the moment GitHub dispatched the run, and it is one-sided by
// design, because only a step that began BEFORE its own run is impossible. A
// host whose clock runs ahead fails none of it. Every step it stamps is
// ordered against the step before it, every step begins comfortably after the
// run was dispatched, and the report ends on "every step on every runner is
// time-ordered" over a host that measures every time-budgeted gate wrong.
// run_window.go says as much in writing and leaves it: a constant offset is
// arguably the worse of the two faults, because nothing in the job's own
// record disagrees with anything else in it.
//
// The scan carries one more clock that is not the runner's: its own, the one
// it already stakes --hours on when it decides which runs are inside the
// window a caller asked for. A run is read here only after GitHub called it
// completed, so both of a step's stamps describe something that has already
// finished. A stamp in the scan's future is therefore the same shape of
// finding as a step that began before its run: two clocks disagreeing where
// only the runner's can plausibly be wrong.
//
// One-sided, again, and for the same reason. A stamp in the past says
// nothing: runs are read hours and days after they ran. Only the impossible
// direction is a finding.
//
// One finding per job, as with the run-window rule. A host an hour ahead is
// an hour ahead on every step it ran, and forty copies of one fault bury the
// rest of the report under the loudest runner.
func stampedAheadOfTheScan(input job, now time.Time) []finding {
	if now.IsZero() {
		return nil
	}
	// In the runner's own order, so the step this names is the one that ran
	// first rather than whichever one was serialized first.
	for _, item := range stepsInRunnerOrder(input.Steps) {
		stamp, ok := lastStampOf(item)
		if !ok {
			continue
		}
		ahead := stamp.Sub(now)
		if ahead <= scanSkewTolerance {
			continue
		}
		name := item.Name
		if name == "" {
			name = "?"
		}
		return []finding{{
			kind: "stamped-after-the-scan-that-read-it", step: name,
			detail: fmt.Sprintf("scan read at %s, '%s' stamped %s",
				now.UTC().Format(time.RFC3339), name, stamp.UTC().Format(time.RFC3339)),
			seconds: ahead.Seconds(), at: stamp,
		}}
	}
	return nil
}

// lastStampOf reports the latest moment a step claims for itself, and whether
// it claims one at all.
//
// The later of the two stamps is the one to judge, because it is the one a
// correct clock could not have put in the future. Which field holds it is not
// assumed: a step that finished before it started is a finding scanJob
// already reports, and reading completed_at blindly would let that same
// broken pair hide a start stamped days ahead. A step with one stamp is
// judged on the one it has, and a skipped step with neither is not evidence
// about any clock.
func lastStampOf(item step) (time.Time, bool) {
	start, hasStart := parseTimestamp(item.StartedAt)
	end, hasEnd := parseTimestamp(item.CompletedAt)
	switch {
	case hasStart && hasEnd:
		if end.After(start) {
			return end, true
		}
		return start, true
	case hasEnd:
		return end, true
	case hasStart:
		return start, true
	}
	return time.Time{}, false
}
