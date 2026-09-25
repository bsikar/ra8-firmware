package boardclient

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

const testPlanID = "018f3a2b-7c41-7a2e-8c9d-3b5f6a7c8d90"

func recoveringBoard(t *testing.T) board.Snapshot {
	t.Helper()
	return transition(t, activeBoard(t), board.AgentUnavailable{Actor: "board-agent", Reason: "monotonic clock continuity lost"})
}

func TestStartRecoveryCarriesThePlanAgainstTheVersionItRead(t *testing.T) {
	state := recoveringBoard(t)
	if state.Phase != board.RecoveryRequired {
		t.Fatalf("fixture phase: got %s, want recovery_required", state.Phase)
	}
	started := transition(t, state, board.BeginRecovery{Actor: "operator", PlanID: testPlanID, Reason: "holder died mid-lease"})
	var posted struct {
		ExpectedVersion uint64 `json:"expected_version"`
		PlanID          string `json:"plan_id"`
		Why             string `json:"why"`
	}
	reads, starts := 0, 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			reads++
			jsonResponse(w, http.StatusOK, state)
			return
		}
		starts++
		if r.URL.Path != "/v1/boards/ek-ra8d2/recovery/start" {
			t.Errorf("recovery went to the wrong path: %s", r.URL.Path)
		}
		if err := json.NewDecoder(r.Body).Decode(&posted); err != nil {
			t.Errorf("recovery body did not decode: %v", err)
		}
		jsonResponse(w, http.StatusOK, map[string]any{"snapshot": started, "events": []board.Event{}})
	})
	defer closeServer()

	snapshot, err := c.StartRecovery(context.Background(), "ek-ra8d2", testPlanID, "holder died mid-lease")
	if err != nil || reads != 1 || starts != 1 {
		t.Fatalf("recovery did not start: reads=%d starts=%d err=%v", reads, starts, err)
	}
	if posted.ExpectedVersion != state.Version || posted.PlanID != testPlanID || posted.Why != "holder died mid-lease" {
		t.Fatalf("the plan was not sent as the operator named it: %+v", posted)
	}
	if snapshot.Phase != board.Recovering {
		t.Fatalf("phase after starting: got %s, want recovering", snapshot.Phase)
	}
}

// A quarantined board is also waiting for a reviewed plan, and it is the case
// an operator most often has in hand.
func TestStartRecoveryWorksOnAQuarantinedBoard(t *testing.T) {
	state := transition(t, activeBoard(t), board.Quarantine{Actor: "operator", Reason: "fixture smells of smoke"})
	started := transition(t, state, board.BeginRecovery{Actor: "operator", PlanID: testPlanID, Reason: "swap the fixture"})
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		jsonResponse(w, http.StatusOK, map[string]any{"snapshot": started, "events": []board.Event{}})
	})
	defer closeServer()

	if _, err := c.StartRecovery(context.Background(), "ek-ra8d2", testPlanID, "swap the fixture"); err != nil {
		t.Fatalf("quarantined board: %v", err)
	}
}

// An operator who names the wrong board is told what is wrong with it, and
// nothing is posted.
func TestStartRecoveryRefusesABoardThatIsNotWaitingForAPlan(t *testing.T) {
	state := activeBoard(t)
	posts := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		posts++
		jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
	})
	defer closeServer()

	if _, err := c.StartRecovery(context.Background(), "ek-ra8d2", testPlanID, "nothing is wrong"); !errors.Is(err, ErrNoRecoveryPending) {
		t.Fatalf("healthy board: got %v, want ErrNoRecoveryPending", err)
	}
	if posts != 0 {
		t.Fatalf("a healthy board was sent a recovery plan %d times", posts)
	}
}

// The plan is the reviewed plan's identifier. Recovery with no plan named is
// not a recovery, and it never reaches the server.
func TestStartRecoveryRefusesAnUnnamedPlanOrReason(t *testing.T) {
	requests := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		requests++
		jsonResponse(w, http.StatusOK, map[string]any{})
	})
	defer closeServer()

	for name, call := range map[string]func() error{
		"no board":  func() error { _, err := c.StartRecovery(context.Background(), "", testPlanID, "why"); return err },
		"no plan":   func() error { _, err := c.StartRecovery(context.Background(), "ek-ra8d2", "", "why"); return err },
		"bad plan":  func() error { _, err := c.StartRecovery(context.Background(), "ek-ra8d2", "plan-1", "why"); return err },
		"no reason": func() error { _, err := c.StartRecovery(context.Background(), "ek-ra8d2", testPlanID, ""); return err },
		"loose reason": func() error {
			_, err := c.StartRecovery(context.Background(), "ek-ra8d2", testPlanID, " padded ")
			return err
		},
	} {
		if err := call(); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s: got %v, want ErrInvalidRequest", name, err)
		}
	}
	if requests != 0 {
		t.Fatalf("a refused recovery reached the server %d times", requests)
	}
}

// The version moved between the read and the start. That is the board being
// busy, not the plan being wrong, so it is retried rather than surfaced.
func TestStartRecoveryRetriesAVersionConflict(t *testing.T) {
	state := recoveringBoard(t)
	started := transition(t, state, board.BeginRecovery{Actor: "operator", PlanID: testPlanID, Reason: "retry me"})
	attempts := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		attempts++
		if attempts == 1 {
			jsonResponse(w, http.StatusConflict, map[string]any{"title": "board version moved"})
			return
		}
		jsonResponse(w, http.StatusOK, map[string]any{"snapshot": started, "events": []board.Event{}})
	})
	defer closeServer()

	if _, err := c.StartRecovery(context.Background(), "ek-ra8d2", testPlanID, "retry me"); err != nil {
		t.Fatalf("conflict was not retried: %v", err)
	}
	if attempts != 2 {
		t.Fatalf("attempts: got %d, want 2", attempts)
	}
}
