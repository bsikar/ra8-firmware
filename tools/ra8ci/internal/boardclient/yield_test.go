package boardclient

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func TestWaitForYieldRequestObservesDurablePriorityChange(t *testing.T) {
	state, _ := board.New("ek-ra8d2")
	state = transition(t, state, board.Enqueue{Actor: "agent", Waiter: board.Waiter{
		ID: testRequestID, LeaseID: testLeaseID, Holder: "agent", Class: board.ClassAI,
		Reason: "automated HIL", Duration: time.Minute,
	}})
	state = transition(t, state, board.AcknowledgeGrant{Actor: "board-agent", LeaseID: testLeaseID,
		Generation: state.Generation, InstalledGeneration: state.Generation})
	token := testToken(state)
	polls := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.URL.Path != "/v1/boards/ek-ra8d2" {
			t.Errorf("unexpected yield poll %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
			return
		}
		polls++
		if polls == 2 {
			state = transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
				ID: testProofID, LeaseID: "01996f90-3415-7cfe-8ff1-600058131b00", Holder: "human",
				Class: board.ClassHuman, Reason: "manual board work", Duration: time.Minute,
			}})
		}
		jsonResponse(w, http.StatusOK, state)
	})
	defer closeServer()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	snapshot, err := c.WaitForYieldRequest(ctx, token)
	if err != nil || snapshot.Phase != board.YieldRequested || polls != 2 {
		t.Fatalf("yield request not observed from server state: phase=%s polls=%d err=%v", snapshot.Phase, polls, err)
	}
	if snapshot.Lease == nil || snapshot.Lease.ID != token.LeaseID || snapshot.Lease.Generation != token.Generation {
		t.Fatalf("yield response was not bound to held lease: %+v", snapshot)
	}
}

func TestWaitForYieldRequestStopsOnCancellation(t *testing.T) {
	state := activeBoard(t)
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, http.StatusOK, state)
	})
	defer closeServer()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := c.WaitForYieldRequest(ctx, testToken(state)); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled yield wait continued: %v", err)
	}
}
