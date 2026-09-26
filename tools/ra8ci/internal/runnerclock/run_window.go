// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"fmt"
	"time"
)

// scanJob answers every question this scanner asks by holding runner
// timestamps against other runner timestamps from the same runner. That finds
// a clock that MOVED underneath a job, which is the fault #509 recorded: one
// step ends after the next begins, or ends before its own start, because the
// host stepped its wall clock mid-job.
//
// It cannot find a clock that was ALREADY wrong when the job began. A runner
// an hour ahead stamps every step an hour ahead, each step is ordered against
// the one before it, and the report says every step on every runner is
// time-ordered. That host is the same hazard the report describes at the end:
// every time-budgeted gate that lands on it measures the wrong thing. A
// constant offset is arguably the worse of the two, because nothing in the
// job's own record disagrees with anything else in it.
//
// The run carries the one clock in this data that is not the runner's.
// run_started_at is stamped by GitHub when the run is dispatched, so a step
// that began before its own run began is not an ordering this scanner has to
// reason about: it is two clocks disagreeing, and only the runner's can be
// wrong.
//
// The rule is ONE-SIDED, and deliberately. A step starting long AFTER the run
// started is ordinary: jobs queue, they wait on a concurrency group, they wait
// on a needs: dependency, and an hour between dispatch and the first step says
// nothing about the host's clock. Only the impossible direction is a finding.
func jobBeganWithinItsRun(input job, runStarted time.Time, haveRunStart bool) []finding {
	if !haveRunStart {
		return nil
	}
	var findings []finding
	for _, item := range input.Steps {
		start, hasStart := parseTimestamp(item.StartedAt)
		if !hasStart {
			continue
		}
		ahead := runStarted.Sub(start)
		if ahead <= overlapTolerance {
			continue
		}
		name := item.Name
		if name == "" {
			name = "?"
		}
		findings = append(findings, finding{
			kind: "began-before-the-run-that-dispatched-it", step: name,
			detail: fmt.Sprintf("run began %s, '%s' began %s",
				runStarted.UTC().Format(time.RFC3339), name, start.UTC().Format(time.RFC3339)),
			seconds: -ahead.Seconds(), at: start,
		})
		// One finding per job. A host whose clock is a constant offset behind
		// reports it on every step it ran, and forty copies of one fault
		// buries the rest of the report under the loudest runner.
		break
	}
	return findings
}

// runStart reads the moment GitHub dispatched a run, preferring the field that
// names it and falling back the way the run walk already does: created_at is
// what the API fills in for runs old enough to predate run_started_at.
func runStart(item run) (time.Time, bool) {
	if started, ok := parseTimestamp(item.RunStarted); ok {
		return started, true
	}
	return parseTimestamp(item.CreatedAt)
}
