package store

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func yieldBudgetHIL(mutate ...func(*catalog.HILTask)) json.RawMessage {
	hil := catalog.HILTask{
		BoardID:                    "ek-ra8d2",
		BoardModel:                 "EK-RA8D2",
		ManifestPath:               "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily:              "uart-demo",
		Mode:                       "uart_scrape",
		ObservationStep:            "observe",
		FlashRestoreSeconds:        10,
		SafetyMaximumSeconds:       120,
		HandoffSafeStepSeconds:     18,
		HandoffRestoreProbeSeconds: 12,
	}
	for _, m := range mutate {
		m(&hil)
	}
	raw, err := json.Marshal(struct {
		Argv []string        `json:"argv"`
		HIL  catalog.HILTask `json:"hil"`
	}{Argv: []string{}, HIL: hil})
	if err != nil {
		panic(err)
	}
	return raw
}

func yieldBudgetWork(mutate ...func(*heldYieldWork)) heldYieldWork {
	work := heldYieldWork{
		ApprovedRevision: "fixture-9",
		SessionRevision:  "fixture-9",
		TaskName:         "hil-blink",
		CatalogDigest:    strings.Repeat("a", 64),
		ImageSHA256:      strings.Repeat("b", 64),
		HIL:              yieldBudgetHIL(),
	}
	for _, m := range mutate {
		m(&work)
	}
	return work
}

func TestYieldWorkBudgetDerivesTheCohortFromTrustedStateOnly(t *testing.T) {
	cohort, bounds, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork())
	if err != nil {
		t.Fatalf("yieldWorkBudget: %v", err)
	}
	want := board.YieldCohort{
		BoardID:         "ek-ra8d2",
		BoardModel:      "EK-RA8D2",
		FixtureRevision: "fixture-9",
		TaskName:        "hil-blink",
		CatalogDigest:   strings.Repeat("a", 64),
		ImageSHA256:     strings.Repeat("b", 64),
	}
	if cohort != want {
		t.Fatalf("cohort = %+v, want %+v", cohort, want)
	}
	if bounds.SafeStepBound != 18*time.Second || bounds.RestoreProbeBound != 12*time.Second {
		t.Fatalf("bounds = %+v", bounds)
	}
	if err := board.ValidateHandoffBounds(bounds); err != nil {
		t.Fatalf("derived bounds refused by the estimator: %v", err)
	}
}

// The cohort a requester's ETA rests on must key on the fixture revision the
// operator approved, never on the one the live session reports. Otherwise a
// session on a different fixture inherits the approved fixture's history.
func TestYieldWorkBudgetRefusesASessionOnADifferentFixture(t *testing.T) {
	_, _, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork(func(w *heldYieldWork) {
		w.SessionRevision = "fixture-10"
	}))
	if !errors.Is(err, ErrDenied) {
		t.Fatalf("err = %v, want ErrDenied", err)
	}
}

func TestYieldWorkBudgetRefusesABoardWithNoApprovedProfile(t *testing.T) {
	_, _, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork(func(w *heldYieldWork) {
		w.ApprovedRevision = ""
		w.SessionRevision = ""
	}))
	if !errors.Is(err, ErrDenied) {
		t.Fatalf("err = %v, want ErrDenied", err)
	}
}

func TestYieldWorkBudgetRefusesWorkReviewedForAnotherBoard(t *testing.T) {
	_, _, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork(func(w *heldYieldWork) {
		w.HIL = yieldBudgetHIL(func(h *catalog.HILTask) { h.BoardID = "ek-ra8d1" })
	}))
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("err = %v, want ErrConflict", err)
	}
}

func TestYieldWorkBudgetRefusesATaskWithNoReviewedHILDefinition(t *testing.T) {
	for _, tc := range []struct {
		name string
		raw  json.RawMessage
	}{
		{"no hil key", json.RawMessage(`{"argv":[]}`)},
		{"null hil", json.RawMessage(`{"argv":[],"hil":null}`)},
		{"not an object", json.RawMessage(`[]`)},
		{"unreviewed metadata", yieldBudgetHIL(func(h *catalog.HILTask) { h.BoardModel = " bad" })},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, _, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork(func(w *heldYieldWork) {
				w.HIL = tc.raw
			}))
			if !errors.Is(err, ErrConflict) {
				t.Fatalf("err = %v, want ErrConflict", err)
			}
		})
	}
}

// PlanYield, not this derivation, decides what an undeclared bound means: a
// person may proceed with an unknown ETA and only automatic dispatch is
// refused. Refusing here would take that judgement away from where it is
// stated, and would close the yield door on every task reviewed before the
// bounds existed.
func TestYieldWorkBudgetReportsUndeclaredBoundsRatherThanRefusing(t *testing.T) {
	cohort, bounds, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork(func(w *heldYieldWork) {
		w.HIL = yieldBudgetHIL(func(h *catalog.HILTask) {
			h.HandoffSafeStepSeconds = 0
			h.HandoffRestoreProbeSeconds = 0
		})
	}))
	if err != nil {
		t.Fatalf("yieldWorkBudget: %v", err)
	}
	if cohort.TaskName != "hil-blink" {
		t.Fatalf("cohort = %+v", cohort)
	}
	if (bounds != board.DeclaredHandoffBounds{}) {
		t.Fatalf("bounds = %+v, want zero", bounds)
	}
	plan, err := board.PlanYield(yieldBudgetSnapshot(), "waiter-1", board.YieldOperator,
		cohort, bounds, nil, time.Now().UTC())
	if err != nil {
		t.Fatalf("PlanYield for an operator: %v", err)
	}
	if plan.Known() {
		t.Fatalf("plan reports a known ETA with no declared bounds: %+v", plan)
	}
	if _, err := board.PlanYield(yieldBudgetSnapshot(), "waiter-1", board.YieldAutomatic,
		cohort, bounds, nil, time.Now().UTC()); err == nil {
		t.Fatal("automatic dispatch accepted with no declared bounds")
	}
}

// An imageless task keeps an empty image digest: it is part of the identity,
// not a missing value, so such a task never borrows an imaged task's history.
func TestYieldWorkBudgetKeepsAnEmptyImageDigestAsIdentity(t *testing.T) {
	cohort, _, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork(func(w *heldYieldWork) {
		w.ImageSHA256 = ""
	}))
	if err != nil {
		t.Fatalf("yieldWorkBudget: %v", err)
	}
	if cohort.ImageSHA256 != "" {
		t.Fatalf("image digest = %q, want empty", cohort.ImageSHA256)
	}
}

func TestYieldWorkBudgetRefusesAnInvalidBoardID(t *testing.T) {
	if _, _, err := yieldWorkBudget("", yieldBudgetWork()); !errors.Is(err, ErrInvalid) {
		t.Fatalf("err = %v, want ErrInvalid", err)
	}
}

// The derived cohort must satisfy the same read the history query runs, or a
// board could be quoted an ETA over history nothing can look up.
func TestYieldWorkBudgetProducesACohortTheHistoryReadAccepts(t *testing.T) {
	cohort, _, err := yieldWorkBudget("ek-ra8d2", yieldBudgetWork())
	if err != nil {
		t.Fatalf("yieldWorkBudget: %v", err)
	}
	if _, err := yieldHistoryArgs(cohort, time.Now().UTC()); err != nil {
		t.Fatalf("history read refused the derived cohort: %v", err)
	}
}

func yieldBudgetSnapshot() board.Snapshot {
	now := time.Now().UTC()
	return board.Snapshot{
		BoardID:        "ek-ra8d2",
		Phase:          board.Active,
		Generation:     1,
		AgentHighWater: 1,
		Version:        4,
		NextSequence:   2,
		Lease: &board.Lease{
			ID: "lease-1", WaiterID: "waiter-0", Holder: "holder-1", Class: board.ClassCI,
			Reason: "hil run", Generation: 1, GrantedAt: now.Add(-time.Minute),
			ExpiresAt: now.Add(30 * time.Minute), RequestedDuration: 30 * time.Minute,
			DeadlineVersion: 1,
		},
		Queue: []board.Waiter{{ID: "waiter-1", LeaseID: "lease-2", Holder: "person-1",
			Class: board.ClassHuman, Reason: "bench time", Duration: 30 * time.Minute,
			Sequence: 1, QueuedAt: now.Add(-time.Second)}},
	}
}
