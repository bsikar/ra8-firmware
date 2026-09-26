// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"context"
	"fmt"
	"net/url"
	"strconv"
)

// maxJobPage is where the GitHub API stops enlarging a page of jobs, and
// maxRunJobs is the point past which a run's job list is not a matrix any
// more and a walk of it is a runaway.
const (
	maxJobPage = 100
	maxRunJobs = 10000
)

// runJobs reads every job of a run, not the first page of them.
//
// The run walk learned this already: one request cannot answer for more than
// maxRunPage runs, so a scan wider than a page reads the pages behind it. The
// jobs of a run were still read with a single request carrying per_page=100,
// which is the same mistake one level down. A matrix workflow passes 100 jobs
// without trying, and this repository's own CI fans out per board, per
// toolchain and per gate.
//
// What that costs is worse here than a short run list, because nothing in the
// output says anything was left out. The scan counts the jobs it read, and
// the report ends with "every step on every runner is time-ordered", a
// sentence about every runner in the window. A runner whose clock stepped
// mid-job sat on job 137 of a matrix, was never read, and the gate it holds
// went on measuring the wrong thing under a clean report. #509 is a fault
// that shows up on ONE host at a time, so the jobs most worth reading are
// exactly the ones a truncated list drops.
//
// The walk ends the way the run walk ends, on a page the API could not fill,
// and refuses rather than truncates past maxRunJobs: a report on part of a
// run, presented as a report on the run, is the thing this rule exists to
// stop.
func runJobs(ctx context.Context, api *actionsAPI, repo string, runID int64) ([]job, error) {
	endpoint := fmt.Sprintf("repos/%s/actions/runs/%d/jobs", repo, runID)
	collected := make([]job, 0, maxJobPage)
	for page := 1; ; page++ {
		query := url.Values{
			"per_page": {strconv.Itoa(maxJobPage)},
			"page":     {strconv.Itoa(page)},
		}
		var payload jobList
		if err := api.get(ctx, endpoint, query, &payload); err != nil {
			return nil, err
		}
		collected = append(collected, payload.Jobs...)
		if len(payload.Jobs) < maxJobPage {
			return collected, nil
		}
		if len(collected) >= maxRunJobs {
			return nil, fmt.Errorf("run %d lists more than %d jobs; refusing to report on part of it", runID, maxRunJobs)
		}
	}
}
