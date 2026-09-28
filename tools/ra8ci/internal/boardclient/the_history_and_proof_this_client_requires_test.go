// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Timing history is what a deadline is derived from, and a neutral proof is
// what says the hardware is safe to hand on. Neither is taken on trust: the
// history has to be about the task that was asked for, and a release or a
// recovery completion is not sent at all when nothing can produce a proof.

func observedTask() catalog.Task {
	return catalog.Task{Name: "uart-demo", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "exclusive",
		Retry: catalog.RetryPolicy{MaxAttempts: 1}, Steps: []catalog.Step{{Name: "observe", Program: "noop"}},
		HIL: &catalog.HILTask{BoardID: "ek-ra8d2", BoardModel: "EK-RA8D2",
			ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/uart_hello/hil.conf",
			ProgramFamily: "uart-hello", Mode: "uart_scrape", ObservationStep: "observe", FlashRestoreSeconds: 10}}
}

func observedWorkload(task catalog.Task) hilspec.Workload {
	return hilspec.Workload{ManifestPath: task.HIL.ManifestPath, BoardModel: task.HIL.BoardModel,
		FixtureRevision: "fixture-v2", ProfileSHA256: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		ProgramFamily: task.HIL.ProgramFamily, Mode: hilspec.Mode(task.HIL.Mode)}
}

// standingProducer is a producer that exists but is never reached: every
// test using it asserts the call stopped before a challenge was minted.
type standingProducer struct{}

func (standingProducer) ProduceNeutralReceipt(context.Context, store.NeutralChallenge) ([]byte, error) {
	return nil, errors.New("no fixture is attached in this test")
}

// servingHistory answers every history request with one document.
func servingHistory(t *testing.T, document any) (*Client, func()) {
	t.Helper()
	return testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusOK, document)
	})
}

// A task this client cannot ask about is refused before a request is spent:
// the server derives the workload from the task name, so a name that is not
// a reviewed HIL task for this board would be answered about something else.
func TestHILObservationsRefusesATaskItCannotAskAbout(t *testing.T) {
	sound := observedTask()
	otherBoard := observedTask()
	otherBoard.HIL.BoardID = "ek-ra8m1"
	notHIL := observedTask()
	notHIL.Scope = "host"
	noBlock := observedTask()
	noBlock.HIL = nil
	unreviewed := observedTask()
	unreviewed.Tier = "whenever"

	client, done := servingHistory(t, map[string]any{})
	defer done()

	for name, ask := range map[string]struct {
		boardID string
		task    catalog.Task
	}{
		"a board ID that is not one":   {"not a board id", sound},
		"another board's HIL block":    {"ek-ra8d2", otherBoard},
		"a task that is not HIL":       {"ek-ra8d2", notHIL},
		"a HIL task with no HIL block": {"ek-ra8d2", noBlock},
		"a task the catalog refuses":   {"ek-ra8d2", unreviewed},
	} {
		if _, _, err := client.HILObservations(context.Background(), ask.boardID, ask.task); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// A history document has to be about the task that was asked for. Accepting
// one that is not would derive this task's deadline from another workload's
// timings, which is how a bounded operation gets the wrong bound.
func TestHILObservationsRefusesAHistoryAboutAnotherWorkload(t *testing.T) {
	task := observedTask()
	sound := observedWorkload(task)

	for name, mutate := range map[string]func(hilspec.Workload) hilspec.Workload{
		"another manifest":       func(w hilspec.Workload) hilspec.Workload { w.ManifestPath = "other/hil.conf"; return w },
		"another board model":    func(w hilspec.Workload) hilspec.Workload { w.BoardModel = "EK-RA8M1"; return w },
		"another program family": func(w hilspec.Workload) hilspec.Workload { w.ProgramFamily = "rtt-hello"; return w },
		"another mode":           func(w hilspec.Workload) hilspec.Workload { w.Mode = hilspec.ModeRTTScrape; return w },
		"no fixture revision":    func(w hilspec.Workload) hilspec.Workload { w.FixtureRevision = ""; return w },
		"no profile digest":      func(w hilspec.Workload) hilspec.Workload { w.ProfileSHA256 = ""; return w },
		"a short profile digest": func(w hilspec.Workload) hilspec.Workload { w.ProfileSHA256 = "eeee"; return w },
		"a shouted profile digest": func(w hilspec.Workload) hilspec.Workload {
			w.ProfileSHA256 = "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE"
			return w
		},
		"a profile digest that is not hex": func(w hilspec.Workload) hilspec.Workload {
			w.ProfileSHA256 = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
			return w
		},
	} {
		client, done := servingHistory(t, map[string]any{
			"task_name": task.Name, "workload": mutate(sound), "observations": []any{},
		})
		if _, _, err := client.HILObservations(context.Background(), "ek-ra8d2", task); err == nil {
			t.Fatalf("%s was accepted as this task's history", name)
		}
		done()
	}

	named, done := servingHistory(t, map[string]any{
		"task_name": "another-task", "workload": sound, "observations": []any{},
	})
	defer done()
	if _, _, err := named.HILObservations(context.Background(), "ek-ra8d2", task); err == nil {
		t.Fatal("history answered about another task was accepted")
	}
}

// Every row has to sit in the cohort the document declared. One row from
// another fixture revision or another profile is enough to move a percentile,
// and a duration outside the bound is not an observation of this task at all.
func TestHILObservationsRefusesARowOutsideTheCohort(t *testing.T) {
	task := observedTask()
	workload := observedWorkload(task)
	sound := hilspec.HistoricalObservation{Workload: workload, Duration: 12 * time.Second,
		Succeeded: true, EvidenceComplete: true}

	for name, mutate := range map[string]func(hilspec.HistoricalObservation) hilspec.HistoricalObservation{
		"another fixture revision": func(r hilspec.HistoricalObservation) hilspec.HistoricalObservation {
			r.Workload.FixtureRevision = "fixture-v1"
			return r
		},
		"another profile": func(r hilspec.HistoricalObservation) hilspec.HistoricalObservation {
			r.Workload.ProfileSHA256 = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
			return r
		},
		"no duration": func(r hilspec.HistoricalObservation) hilspec.HistoricalObservation { r.Duration = 0; return r },
		"a duration before it started": func(r hilspec.HistoricalObservation) hilspec.HistoricalObservation {
			r.Duration = -time.Second
			return r
		},
		"a duration past the hour": func(r hilspec.HistoricalObservation) hilspec.HistoricalObservation {
			r.Duration = time.Hour + time.Second
			return r
		},
	} {
		client, done := servingHistory(t, map[string]any{
			"task_name": task.Name, "workload": workload,
			"observations": []hilspec.HistoricalObservation{sound, mutate(sound)},
		})
		got, rows, err := client.HILObservations(context.Background(), "ek-ra8d2", task)
		if err == nil {
			t.Fatalf("a row with %s was accepted", name)
		}
		if got != (hilspec.Workload{}) || rows != nil {
			t.Fatalf("a refused history still carried %d rows", len(rows))
		}
		done()
	}
}

// A history the server would not serve is handed back as it happened rather
// than as an empty cohort, which a caller would read as "never observed".
func TestHILObservationsHandsBackARefusedHistory(t *testing.T) {
	task := observedTask()
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusServiceUnavailable, map[string]any{
			"code": "unavailable", "detail": "history is rebuilding", "retryable": true,
		})
	})
	defer done()

	if _, rows, err := client.HILObservations(context.Background(), "ek-ra8d2", task); err == nil || rows != nil {
		t.Fatalf("a refused history was answered with %d rows, err=%v", len(rows), err)
	}
}

// With nothing able to neutralize the fixture, no release is sent. Releasing
// a board on the assumption that stopped work left safe hardware is the one
// thing the neutral proof exists to prevent.
func TestFreeSendsNothingWithoutANeutralProducer(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	if _, err := client.Free(context.Background(), testToken(state), nil); !errors.Is(err, ErrNeutralUnavailable) {
		t.Fatalf("release without a producer = %v", err)
	}
	if commands.Load() != 0 {
		t.Fatal("a board was released with nothing able to neutralize it")
	}
}

// A board already past its active life is not this holder's to release.
func TestFreeRefusesAPhaseThatIsNoLongerHeld(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	for name, phase := range awaitingRecovery(t, state) {
		client, _, done := countedBoard(t, phase)
		if _, err := client.Free(context.Background(), token, standingProducer{}); !errors.Is(err, ErrStaleLease) {
			t.Fatalf("%s = %v", name, err)
		}
		done()
	}
}

// A recovery completion is the same promise as a release: without a producer
// none is sent, and a board that is not recovering has none to complete.
func TestFinishRecoverySendsNothingItCannotProve(t *testing.T) {
	recovering := transition(t, transition(t, activeBoard(t),
		board.AgentUnavailable{Actor: "operator", Reason: "agent gone"}),
		board.BeginRecovery{Actor: "operator", PlanID: "plan-1", Reason: "reflash"})
	client, commands, done := countedBoard(t, recovering)
	defer done()

	if _, err := client.FinishRecovery(context.Background(), "not a board id", standingProducer{}); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("a board ID that is not one = %v", err)
	}
	if _, err := client.FinishRecovery(context.Background(), "ek-ra8d2", nil); !errors.Is(err, ErrNeutralUnavailable) {
		t.Fatalf("completion without a producer = %v", err)
	}
	if commands.Load() != 0 {
		t.Fatal("a recovery was completed with nothing able to neutralize it")
	}

	ready, _, doneReady := countedBoard(t, activeBoard(t))
	defer doneReady()
	if _, err := ready.FinishRecovery(context.Background(), "ek-ra8d2", standingProducer{}); !errors.Is(err, ErrNoRecoveryPending) {
		t.Fatalf("completing a recovery nobody started = %v", err)
	}
}

// An unreachable server is handed back as the transport failure it is, not
// as a board with no recovery pending.
func TestFinishRecoveryHandsBackAnUnreachableServer(t *testing.T) {
	client, _, done := countedBoard(t, activeBoard(t))
	done()

	_, err := client.FinishRecovery(context.Background(), "ek-ra8d2", standingProducer{})
	if err == nil || errors.Is(err, ErrNoRecoveryPending) {
		t.Fatalf("an unreachable server was read as board state: %v", err)
	}
}
