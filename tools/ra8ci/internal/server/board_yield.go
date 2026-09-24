package server

import (
	"context"
	"net/http"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Asking a board to yield, over HTTP.
//
// board.RequestYield has been a command the state machine accepts since the
// board landed and no endpoint ever issued it: a queued waiter that outranked
// the holder had no way to say so, and the drain checkpoint the holder posts
// answered a request nobody could make. The estimator (EstimateHandoff), the
// sample recorder (YieldSampleFor) and the plan that joins them (PlanYield)
// had no caller either.
//
// This is that door, and it is deliberately the planning one. The estimate is
// taken BEFORE the board is asked, from the snapshot as it stands, and the
// board is asked only if the plan stands up. A yield dispatched with no answer
// to "how long will this take" is exactly what the dynamic yield budget exists
// to prevent, so the ordering here is the property, not a convenience.

// HandoffBudget is the trusted input to an estimate: which history is
// comparable, what the task declares about its own indivisible work, and the
// samples themselves. None of it comes from the requester.
type HandoffBudget struct {
	Cohort  board.YieldCohort
	Bounds  board.DeclaredHandoffBounds
	Samples []board.YieldSample
}

// BoardYieldBudget supplies the handoff budget for a board about to be asked
// to yield. It takes the snapshot rather than a board ID because the cohort
// depends on what the board is doing right now, not only on which board it is.
type BoardYieldBudget interface {
	HandoffBudget(context.Context, board.Snapshot) (HandoffBudget, error)
}

type yieldRequest struct {
	ExpectedVersion uint64 `json:"expected_version"`
	WaiterID        string `json:"waiter_id"`
}

// yieldDispatch decides which standard this request is held to, from the
// class the board itself records for the named waiter.
//
// The requester does not get to state this. PlanYield refuses an automatic
// dispatch whose task declares no handoff bounds and lets a person proceed
// with an unknown ETA; a dispatch field on the wire would be a one-word way
// around that refusal. An unknown waiter is treated as automatic, the
// stricter of the two, and is refused by admission a moment later anyway.
func yieldDispatch(s board.Snapshot, waiterID string) board.YieldDispatch {
	for _, w := range s.Queue {
		if w.ID == waiterID {
			if w.Class == board.ClassHuman {
				return board.YieldOperator
			}
			return board.YieldAutomatic
		}
	}
	return board.YieldAutomatic
}

func (h *boardHTTP) yield(w http.ResponseWriter, r *http.Request) {
	actor, ok := h.authorize(w, r, "board.yield")
	if !ok {
		return
	}
	var req yieldRequest
	if !decodeBoardJSON(w, r, &req) {
		return
	}
	if !store.ValidID(req.WaiterID) {
		problem(w, http.StatusBadRequest, "invalid_argument", "invalid waiter ID", false)
		return
	}
	// Same shape as the missing neutral verifier on release: a dependency the
	// deployment has not configured leaves the door closed rather than open
	// with the safeguard silently skipped.
	if h.budget == nil {
		problem(w, http.StatusServiceUnavailable, "unavailable", "yield budget is not configured", true)
		return
	}
	snapshot, err := h.store.GetBoard(r.Context(), r.PathValue("board_id"))
	if err != nil {
		writeBoardError(w, err)
		return
	}
	budget, err := h.budget.HandoffBudget(r.Context(), snapshot)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	// One clock for the plan and the commit, so the lease's YieldRequestedAt
	// is the instant the requester's ETA is anchored to rather than a few
	// microseconds past it. The target the plan shows is committed with the
	// request for the same reason: the number in the response body and the
	// number the handoff is later judged against are one value, recorded by
	// the transition that made the promise. The cohort it was estimated over
	// is committed with it, so the sample recorded when the handoff ends is
	// filed against the history the requester was actually quoted.
	now := time.Now().UTC()
	plan, err := board.PlanYield(snapshot, req.WaiterID, yieldDispatch(snapshot, req.WaiterID),
		budget.Cohort, budget.Bounds, budget.Samples, now)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	applied, events, err := h.store.ApplyBoardCommand(r.Context(), actor,
		board.RequestYield{Actor: actor.ID(), WaiterID: req.WaiterID,
			ShownTarget: plan.ShownTarget(), Cohort: budget.Cohort},
		req.ExpectedVersion, nil, h.verifier, now)
	if err != nil {
		writeBoardError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Snapshot board.Snapshot `json:"snapshot"`
		Events   []board.Event  `json:"events"`
		Plan     yieldPlanView  `json:"plan"`
	}{Snapshot: applied, Events: events, Plan: newYieldPlanView(plan, now)})
}

// yieldPlanView is the plan as the requester reads it.
//
// Durations are seconds here, unlike the snapshot, which is transferred as
// the Go value it is. The difference is deliberate: a snapshot is state a
// client round-trips, while this is an answer a person reads off a terminal,
// and "45" beats 45000000000 for that.
type yieldPlanView struct {
	Known             bool              `json:"known"`
	Dispatch          string            `json:"dispatch"`
	Outstanding       bool              `json:"outstanding"`
	Overdue           bool              `json:"overdue"`
	RequestedAt       time.Time         `json:"requested_at"`
	ExpectedNeutralAt *time.Time        `json:"expected_neutral_at"`
	TargetSeconds     float64           `json:"target_seconds"`
	Explain           string            `json:"explain"`
	Estimate          yieldEstimateView `json:"estimate"`
}

type yieldEstimateView struct {
	Source            string  `json:"source"`
	Samples           int     `json:"samples"`
	Censored          int     `json:"censored"`
	Stale             int     `json:"stale"`
	Overruns          int     `json:"overruns"`
	Quantile          float64 `json:"quantile"`
	MarginSeconds     float64 `json:"margin_seconds"`
	SafetyBoundSecond float64 `json:"safety_bound_seconds"`
	BoardID           string  `json:"board_id"`
	BoardModel        string  `json:"board_model"`
	FixtureRevision   string  `json:"fixture_revision"`
	TaskName          string  `json:"task_name"`
	CatalogDigest     string  `json:"catalog_digest"`
	ImageSHA256       string  `json:"image_sha256,omitempty"`
}

func newYieldPlanView(plan board.YieldPlan, now time.Time) yieldPlanView {
	view := yieldPlanView{
		Known:         plan.Known(),
		Dispatch:      string(plan.Dispatch),
		Outstanding:   plan.Outstanding,
		Overdue:       plan.Overdue(now),
		RequestedAt:   plan.RequestedAt,
		TargetSeconds: plan.ShownTarget().Seconds(),
		Explain:       plan.Explain(now),
		Estimate: yieldEstimateView{
			Source:            string(plan.Estimate.Source),
			Samples:           plan.Estimate.Samples,
			Censored:          plan.Estimate.Censored,
			Stale:             plan.Estimate.Stale,
			Overruns:          plan.Estimate.Overruns,
			Quantile:          plan.Estimate.Quantile,
			MarginSeconds:     plan.Estimate.Margin.Seconds(),
			SafetyBoundSecond: plan.Estimate.SafetyBound.Seconds(),
			BoardID:           plan.Estimate.Cohort.BoardID,
			BoardModel:        plan.Estimate.Cohort.BoardModel,
			FixtureRevision:   plan.Estimate.Cohort.FixtureRevision,
			TaskName:          plan.Estimate.Cohort.TaskName,
			CatalogDigest:     plan.Estimate.Cohort.CatalogDigest,
			ImageSHA256:       plan.Estimate.Cohort.ImageSHA256,
		},
	}
	// An unknown ETA carries no time at all. A null reads as "we do not know"
	// where the zero time reads as a date in year one, which a client would
	// have to know to special-case.
	if !plan.ExpectedNeutralAt.IsZero() {
		at := plan.ExpectedNeutralAt
		view.ExpectedNeutralAt = &at
	}
	return view
}
