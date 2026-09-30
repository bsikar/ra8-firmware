// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

// heldTask is the reviewed HIL definition as tasks.arguments actually holds
// it: the definition under a "hil" key, raw, exactly as the row carries it.
func heldTask(change func(*catalog.HILTask)) json.RawMessage {
	definition := hilContract()
	if change != nil {
		change(&definition)
	}
	raw, err := json.Marshal(map[string]any{"hil": definition})
	if err != nil {
		panic(err)
	}
	return raw
}

func aHeldWork() heldYieldWork {
	return heldYieldWork{
		ApprovedRevision: "rev-7",
		SessionRevision:  "rev-7",
		TaskName:         "hil-alive",
		CatalogDigest:    strings.Repeat("a", 64),
		ImageSHA256:      strings.Repeat("b", 64),
		HIL:              heldTask(nil),
	}
}

func spoiledWork(change func(*heldYieldWork)) heldYieldWork {
	work := aHeldWork()
	change(&work)
	return work
}

// The yield budget refuses four different ways, and which one a caller gets
// is the whole diagnostic value of the function.
//
// A disagreeing fixture is ErrDenied rather than ErrConflict: the two records
// disagreeing about what is physically on the bench is not a race to retry,
// it is a board nobody should be estimating a handoff for.
func TestTheYieldBudgetNamesWhichRecordDisagrees(t *testing.T) {
	for _, refusal := range []struct {
		what    string
		boardID string
		work    heldYieldWork
		want    error
	}{
		{"no board", "", aHeldWork(), ErrInvalid},
		{"a padded board identifier", " " + doorBoard, aHeldWork(), ErrInvalid},
		{"a board identifier past the column bound", strings.Repeat("b", 129), aHeldWork(), ErrInvalid},

		{"a bench with no approved fixture profile", doorBoard,
			spoiledWork(func(w *heldYieldWork) { w.ApprovedRevision = "" }), ErrDenied},
		{"a session on a different fixture revision", doorBoard,
			spoiledWork(func(w *heldYieldWork) { w.SessionRevision = "rev-8" }), ErrDenied},

		{"arguments that are not JSON", doorBoard,
			spoiledWork(func(w *heldYieldWork) { w.HIL = json.RawMessage("{") }), ErrConflict},
		{"arguments carrying no HIL definition", doorBoard,
			spoiledWork(func(w *heldYieldWork) { w.HIL = json.RawMessage("{}") }), ErrConflict},
		{"an explicitly null definition", doorBoard,
			spoiledWork(func(w *heldYieldWork) { w.HIL = json.RawMessage(`{"hil":null}`) }), ErrConflict},
		{"a definition with no manifest", doorBoard,
			spoiledWork(func(w *heldYieldWork) {
				w.HIL = heldTask(func(h *catalog.HILTask) { h.ManifestPath = "" })
			}), ErrConflict},
		{"a definition reviewed for another bench", doorBoard,
			spoiledWork(func(w *heldYieldWork) {
				w.HIL = heldTask(func(h *catalog.HILTask) { h.BoardID = "bench-two" })
			}), ErrConflict},
		{"a cohort with no task name", doorBoard,
			spoiledWork(func(w *heldYieldWork) { w.TaskName = "" }), ErrConflict},
		{"a cohort with no catalog digest", doorBoard,
			spoiledWork(func(w *heldYieldWork) { w.CatalogDigest = "" }), ErrConflict},
	} {
		_, _, err := yieldWorkBudget(refusal.boardID, refusal.work)
		if !errors.Is(err, refusal.want) {
			t.Fatalf("%s: err %v, want %v", refusal.what, err, refusal.want)
		}
	}
}

// The cohort a budget derives is keyed on the APPROVED fixture revision, the
// task, and the catalog the run was planned against, so history is only ever
// read back for work assembled the same way.
func TestAYieldCohortIsKeyedOnTheApprovedFixture(t *testing.T) {
	cohort, bounds, err := yieldWorkBudget(doorBoard, aHeldWork())
	if err != nil {
		t.Fatalf("a well-formed held task refused: %v", err)
	}
	if cohort.BoardID != doorBoard || cohort.BoardModel != hilContract().BoardModel ||
		cohort.FixtureRevision != "rev-7" || cohort.TaskName != "hil-alive" ||
		cohort.CatalogDigest != strings.Repeat("a", 64) || cohort.ImageSHA256 != strings.Repeat("b", 64) {
		t.Fatalf("cohort %+v does not describe the held work", cohort)
	}
	if bounds.SafeStepBound != 0 || bounds.RestoreProbeBound != 0 {
		t.Fatalf("a task declaring no handoff bounds reported %+v", bounds)
	}
}

// Undeclared handoff bounds are an answer, not a refusal. A person asking for
// the board still gets a plan with the ETA reported unknown; only automatic
// dispatch is refused, and that judgement belongs to board.PlanYield rather
// than to this function.
func TestUndeclaredHandoffBoundsAreAnAnswerNotARefusal(t *testing.T) {
	_, bounds, err := yieldWorkBudget(doorBoard, spoiledWork(func(w *heldYieldWork) {
		w.HIL = heldTask(func(h *catalog.HILTask) {
			h.HandoffSafeStepSeconds, h.HandoffRestoreProbeSeconds = 0, 0
		})
	}))
	if err != nil {
		t.Fatalf("a task with no declared bounds refused: %v", err)
	}
	if bounds.SafeStepBound != 0 || bounds.RestoreProbeBound != 0 {
		t.Fatalf("undeclared bounds came back as %+v", bounds)
	}

	// Half-declared is not quietly read as undeclared: the two bounds are
	// reviewed together or not at all, and a definition carrying one of them
	// alone is refused as a conflict rather than having the missing half
	// defaulted to zero. The rule is enforced upstream, by
	// catalog.ValidateHILTaskMetadata, so this door never has to decide
	// whether a lone safe-step bound is a bound; it is pinned here because
	// the alternative reading would silently quote a handoff ETA below its
	// own safety floor.
	for _, half := range []struct {
		what              string
		safeStep, restore int
	}{
		{"only a safe-step bound", 9, 0},
		{"only a restore-probe bound", 0, 9},
	} {
		_, _, err := yieldWorkBudget(doorBoard, spoiledWork(func(w *heldYieldWork) {
			w.HIL = heldTask(func(h *catalog.HILTask) {
				h.HandoffSafeStepSeconds, h.HandoffRestoreProbeSeconds = half.safeStep, half.restore
			})
		}))
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("%s: err %v, want ErrConflict", half.what, err)
		}
	}

	_, declared, err := yieldWorkBudget(doorBoard, spoiledWork(func(w *heldYieldWork) {
		w.HIL = heldTask(func(h *catalog.HILTask) {
			h.HandoffSafeStepSeconds, h.HandoffRestoreProbeSeconds = 12, 30
		})
	}))
	if err != nil {
		t.Fatalf("a task declaring both bounds refused: %v", err)
	}
	if declared.SafeStepBound != 12*time.Second || declared.RestoreProbeBound != 30*time.Second {
		t.Fatalf("declared bounds came back as %+v", declared)
	}
}

// The terminal task state is not the attempt's own result. A preempted
// attempt is the case that matters: the attempt ended, but the TASK goes back
// to the queue unless the whole run was cancelled, which is what stops a
// preemption from being read as a failure nobody caused.
func TestAPreemptedAttemptReturnsItsTaskToTheQueue(t *testing.T) {
	exit := func(code int) *int { return &code }

	for _, c := range []struct {
		what      string
		in        BoardHILCompletion
		cancelled bool
		want      string
	}{
		{"a preemption under a live run", BoardHILCompletion{Result: "preempted"}, false, "scheduled"},
		{"a preemption under a cancelled run", BoardHILCompletion{Result: "preempted"}, true, "cancelled"},
		{"a success with complete evidence",
			BoardHILCompletion{Result: "succeeded", EvidenceComplete: true, ChildExitCode: exit(0)}, false, "succeeded"},
		{"a success whose evidence never arrived",
			BoardHILCompletion{Result: "succeeded", ChildExitCode: exit(0)}, false, "failed"},
		{"a success with missing evidence under a cancelled run",
			BoardHILCompletion{Result: "succeeded", ChildExitCode: exit(0)}, true, "failed"},
		{"a failure", BoardHILCompletion{Result: "failed"}, false, "failed"},
		{"a timeout", BoardHILCompletion{Result: "timed_out", HitDeadline: true}, false, "timed_out"},
		{"a cancellation", BoardHILCompletion{Result: "cancelled"}, false, "cancelled"},
		{"a cancellation under a cancelled run", BoardHILCompletion{Result: "cancelled"}, true, "cancelled"},
		{"a lost attempt", BoardHILCompletion{Result: "lost"}, false, "lost"},
	} {
		if got := taskResultForHIL(c.in, c.cancelled); got != c.want {
			t.Fatalf("%s: task result %q, want %q", c.what, got, c.want)
		}
	}
}

// Closing is safe on every shape of store a caller can hold, including one
// that was never opened, because Close sits in the defer of a startup path
// that may have failed anywhere.
func TestClosingAnUnopenedStoreIsSafe(t *testing.T) {
	var absent *Store
	absent.Close()
	(&Store{}).Close()

	plane := unreachablePlane(t)
	plane.Close()
	plane.Close() // the cleanup closes it again; twice must stay safe
}

// The schema check refuses an unopened plane before it reads a version, so a
// server that never connected does not report a schema mismatch it could not
// have seen.
func TestTheSchemaCheckRefusesAnUnopenedPlane(t *testing.T) {
	var absent *Store
	if err := absent.CheckSchema(context.Background()); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("a schema check with no store: err %v, want ErrUnavailable", err)
	}
	if err := (&Store{}).CheckSchema(context.Background()); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("a schema check on an unopened plane: err %v, want ErrUnavailable", err)
	}
}

// The stored-observation view hands back exactly the rows it was built from.
// It is the seam that lets the estimator read history from a query result
// without the estimator knowing there was a database, so it must not filter,
// copy, or reorder anything on the way through.
func TestTheStoredObservationViewHandsBackItsOwnRows(t *testing.T) {
	rows := make(hilObservationRows, 3)
	got, err := rows.Observations(context.Background(), hilspec.Workload{})
	if err != nil {
		t.Fatalf("reading held observations: %v", err)
	}
	if len(got) != 3 || &got[0] != &rows[0] {
		t.Fatalf("the view copied or reshaped its rows: len %d", len(got))
	}

	// A nil context and an unrelated workload change nothing: the filtering
	// happened in SQL, and this view is not entitled to second-guess it.
	same, err := rows.Observations(nil, hilspec.Workload{BoardModel: "EK-RA8D2"})
	if err != nil || len(same) != 3 {
		t.Fatalf("the view judged its arguments: len %d, err %v", len(same), err)
	}

	empty, err := hilObservationRows(nil).Observations(context.Background(), hilspec.Workload{})
	if err != nil || len(empty) != 0 {
		t.Fatalf("an empty history: len %d, err %v", len(empty), err)
	}
}
