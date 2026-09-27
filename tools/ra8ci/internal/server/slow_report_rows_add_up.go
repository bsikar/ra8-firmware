// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"math"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Holding a slow report's rows to a summary a reader can act on.
//
// The two paging doors in this package already judge what the store hands
// back before it goes out: a log page has to fall inside the window the
// reader asked for (logPageFitsItsWindow) and an event page has to do the
// same in order (runEventPageIsAscendingInsideTheWindow). The slow report is
// the third read door and it judged its request carefully and its answer not
// at all. Whatever rows came back were written to the response as they were.
//
// Two things can come out of that aggregation that no reader can use.
//
// The first is a number that is not a number. Every duration and load on a
// row is a double the database computed: percentile_cont over a set, an AVG
// over means, a ratio of two AVGs. A division by an amount that turned out to
// be zero, or a stored non-finite sample, lands here as NaN or an infinity,
// and encoding/json refuses to encode either. writeJSON has already written
// 200 and the header by then and the encoder's error is discarded, so one bad
// row does not fail the request: it truncates the body mid-object, and the
// reader gets a success it cannot parse, with nothing in the response saying
// why. That is the worst shape this door has, because the caller's own retry
// returns exactly the same thing.
//
// The second is a row that parses and still cannot be read. The three
// durations on a row are a median, a 95th percentile and a maximum over ONE
// set of attempts, so they are ordered by construction: median <= p95 <= max,
// and none of them negative. MeanHostLoad is an average of per-attempt mean
// loads and PeakHostLoad is the maximum of the per-attempt peaks over the
// same attempts, so the mean cannot sit above the peak. A row that breaks any
// of those is not a slow task described imprecisely, it is arithmetic that
// did not come from the set it claims to summarize, and an operator reads
// these numbers to size hosts and set task deadlines.
//
// So this is a stored-state guard, like its two siblings, not request
// validation: SlowTasks builds every one of these fields from the same
// GROUP BY over the same attempts, and a row arriving here out of order means
// the read path disagrees with itself. The caller answers 503 for that rather
// than 400, and the rule refuses rather than repairing a row: a summary whose
// own arithmetic does not add up is not a summary to hand on with a number
// corrected.
func slowReportRowsAddUp(rows []store.SlowTask) bool {
	for _, row := range rows {
		if !slowReportRowAddsUp(row) {
			return false
		}
	}
	return true
}

func slowReportRowAddsUp(row store.SlowTask) bool {
	// A row exists because COUNT(DISTINCT a.id) found attempts to
	// summarize, so a row claiming none of them is summarizing nothing.
	if row.Samples < 1 || row.ResourceSamples < 0 {
		return false
	}
	for _, value := range []float64{
		row.MedianSeconds, row.P95Seconds, row.MaximumSeconds, row.MeanStartLoad,
		row.MeanHostLoad, row.PeakHostLoad, row.MeanRAMFreeBytes, row.MeanHostCores,
	} {
		if !measured(value) {
			return false
		}
	}
	if row.MedianSeconds > row.P95Seconds || row.P95Seconds > row.MaximumSeconds {
		return false
	}
	if row.MeanHostLoad > row.PeakHostLoad {
		return false
	}
	for _, optional := range []*float64{row.MeanCPUBusyPct, row.MeanLoadPerCore} {
		if optional != nil && !measured(*optional) {
			return false
		}
	}
	// A share of the host's RAM is a share: the query derives it as
	// 100*(1-free/total) over the same attempts, so a value outside the
	// scale means free memory was reported above the total.
	if row.MeanRAMUsedPct != nil && (!measured(*row.MeanRAMUsedPct) || *row.MeanRAMUsedPct > 100) {
		return false
	}
	return true
}

// measured reports whether a double on a report row is a quantity a reader
// can act on: a real number, and not a negative amount of time, load, memory
// or cores.
func measured(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0) && value >= 0
}
