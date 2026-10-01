// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"math"
	"net/http/httptest"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// summarized is the shape the query builds for a task with attempts behind
// it: ordered durations, a peak at or above the mean, and every optional
// share present.
func summarized() store.SlowTask {
	busy, perCore, ramUsed := 42.0, 1.5, 61.25
	return store.SlowTask{
		Name: "build.linux", Tier: "fast", HostClass: "linux-x86",
		Samples: 9, MedianSeconds: 30, P95Seconds: 91.5, MaximumSeconds: 120,
		MeanStartLoad: 1.25, HostOS: "linux", HostLoadKind: "linux_load1",
		ResourceSamples: 44, MeanHostLoad: 1.75, PeakHostLoad: 3.5,
		MeanRAMFreeBytes: 8 << 30, MeanHostCores: 8,
		MeanCPUBusyPct: &busy, MeanLoadPerCore: &perCore, MeanRAMUsedPct: &ramUsed,
	}
}

func withRow(mutate func(*store.SlowTask)) store.SlowTask {
	row := summarized()
	mutate(&row)
	return row
}

func share(value float64) *float64 { return &value }

func TestSlowReportRowJudgesEachShape(t *testing.T) {
	cases := []struct {
		name    string
		row     store.SlowTask
		accepts bool
	}{
		{"a summarized task", summarized(), true},
		{"one attempt, all three durations equal", withRow(func(r *store.SlowTask) {
			r.Samples, r.MedianSeconds, r.P95Seconds, r.MaximumSeconds = 1, 7, 7, 7
		}), true},
		{"a task that took no measurable time", withRow(func(r *store.SlowTask) {
			r.MedianSeconds, r.P95Seconds, r.MaximumSeconds = 0, 0, 0
		}), true},
		{"no resource samples, so the loads coalesce to zero", withRow(func(r *store.SlowTask) {
			r.ResourceSamples, r.MeanHostLoad, r.PeakHostLoad = 0, 0, 0
			r.MeanRAMFreeBytes, r.MeanCPUBusyPct, r.MeanLoadPerCore, r.MeanRAMUsedPct = 0, nil, nil, nil
		}), true},
		{"a host with no free memory left", withRow(func(r *store.SlowTask) {
			r.MeanRAMFreeBytes, r.MeanRAMUsedPct = 0, share(100)
		}), true},
		{"no attempts behind the row", withRow(func(r *store.SlowTask) { r.Samples = 0 }), false},
		{"a negative count of attempts", withRow(func(r *store.SlowTask) { r.Samples = -1 }), false},
		{"a negative count of resource samples", withRow(func(r *store.SlowTask) { r.ResourceSamples = -1 }), false},
		{"a median above its own 95th percentile", withRow(func(r *store.SlowTask) { r.MedianSeconds = 100 }), false},
		{"a 95th percentile above the maximum", withRow(func(r *store.SlowTask) { r.P95Seconds = 121 }), false},
		{"a maximum below the median", withRow(func(r *store.SlowTask) { r.MaximumSeconds = 1 }), false},
		{"a negative duration", withRow(func(r *store.SlowTask) { r.MedianSeconds = -1 }), false},
		{"a mean load above the peak", withRow(func(r *store.SlowTask) { r.MeanHostLoad = 4 }), false},
		{"a negative peak load", withRow(func(r *store.SlowTask) { r.MeanHostLoad, r.PeakHostLoad = -2, -1 }), false},
		{"a negative amount of free memory", withRow(func(r *store.SlowTask) { r.MeanRAMFreeBytes = -1 }), false},
		{"a negative core count", withRow(func(r *store.SlowTask) { r.MeanHostCores = -1 }), false},
		{"a negative start load", withRow(func(r *store.SlowTask) { r.MeanStartLoad = -0.5 }), false},
		{"a median that is not a number", withRow(func(r *store.SlowTask) { r.MedianSeconds = math.NaN() }), false},
		{"an unbounded maximum", withRow(func(r *store.SlowTask) { r.MaximumSeconds = math.Inf(1) }), false},
		{"a peak load that is not a number", withRow(func(r *store.SlowTask) { r.PeakHostLoad = math.NaN() }), false},
		{"a busy share that is not a number", withRow(func(r *store.SlowTask) { r.MeanCPUBusyPct = share(math.NaN()) }), false},
		{"an unbounded load per core", withRow(func(r *store.SlowTask) { r.MeanLoadPerCore = share(math.Inf(1)) }), false},
		{"a negative load per core", withRow(func(r *store.SlowTask) { r.MeanLoadPerCore = share(-1) }), false},
		{"more memory used than the host has", withRow(func(r *store.SlowTask) { r.MeanRAMUsedPct = share(100.5) }), false},
		{"more memory free than the host has", withRow(func(r *store.SlowTask) { r.MeanRAMUsedPct = share(-3) }), false},
		{"a RAM share that is not a number", withRow(func(r *store.SlowTask) { r.MeanRAMUsedPct = share(math.NaN()) }), false},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			if got := slowReportRowsAddUp([]store.SlowTask{testCase.row}); got != testCase.accepts {
				t.Fatalf("accepts=%v want %v", got, testCase.accepts)
			}
		})
	}
}

func TestSlowReportJudgesEveryRowOnThePage(t *testing.T) {
	if !slowReportRowsAddUp(nil) {
		t.Fatal("a report with no rows was refused")
	}
	if !slowReportRowsAddUp([]store.SlowTask{summarized(), summarized(), summarized()}) {
		t.Fatal("a page of summarized rows was refused")
	}
	broken := withRow(func(r *store.SlowTask) { r.MedianSeconds = math.NaN() })
	for _, position := range []int{0, 1, 2} {
		page := []store.SlowTask{summarized(), summarized(), summarized()}
		page[position] = broken
		if slowReportRowsAddUp(page) {
			t.Fatalf("a page carrying an unreadable row at %d was accepted", position)
		}
	}
}

// The reason the non-finite cases are refused rather than passed on: the
// response writer has already sent 200 by the time the encoder sees them, and
// its error is discarded, so the reader is handed a success with a body that
// stops mid-object.
func TestAnUnreadableRowTruncatesTheResponseBody(t *testing.T) {
	recorder := httptest.NewRecorder()
	writeJSON(recorder, 200, slowReportResponse{Repository: "bsikar/ra8-firmware", WindowSeconds: 3600,
		Tasks: []store.SlowTask{withRow(func(r *store.SlowTask) { r.MedianSeconds = math.NaN() })}})
	var decoded slowReportResponse
	if err := json.Unmarshal(recorder.Body.Bytes(), &decoded); err == nil {
		t.Fatal("a row carrying NaN encoded into a body a reader could parse")
	}
	if recorder.Code != 200 {
		t.Fatalf("status=%d, so the reader was told something was wrong", recorder.Code)
	}
}
