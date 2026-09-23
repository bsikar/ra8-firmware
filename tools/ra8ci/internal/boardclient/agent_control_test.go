package boardclient

import (
	"context"
	"encoding/json"
	"net/http"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func TestAcknowledgeGrantPersistsExactInstalledGeneration(t *testing.T) {
	state, _ := board.New("ek-ra8d2")
	state = transition(t, state, board.Enqueue{Actor: "agent", Waiter: board.Waiter{
		ID: testRequestID, LeaseID: testLeaseID, Holder: "agent", Class: board.ClassAI,
		Reason: "HIL", Duration: 60_000_000_000,
	}})
	token := testToken(state)
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/agent/ack":
			var request struct {
				ExpectedVersion     uint64 `json:"expected_version"`
				LeaseID             string `json:"lease_id"`
				Generation          uint64 `json:"generation"`
				InstalledGeneration uint64 `json:"installed_generation"`
			}
			if err := json.NewDecoder(r.Body).Decode(&request); err != nil ||
				request.ExpectedVersion != state.Version || request.LeaseID != token.LeaseID ||
				request.Generation != token.Generation || request.InstalledGeneration != token.Generation {
				t.Errorf("grant acknowledgement mismatch: %+v err=%v", request, err)
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			state = transition(t, state, board.AcknowledgeGrant{Actor: "board-agent",
				LeaseID: token.LeaseID, Generation: token.Generation,
				InstalledGeneration: token.Generation})
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
		default:
			t.Errorf("unexpected route %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer closeServer()
	result, err := c.AcknowledgeGrant(context.Background(), token)
	if err != nil || result.Phase != board.Active || result.AgentHighWater != token.Generation {
		t.Fatalf("durable generation was not acknowledged: %+v %v", result, err)
	}
}

func TestObserveAgentGenerationQuarantinesDatabaseRollback(t *testing.T) {
	state, _ := board.New("ek-ra8d2")
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/agent/observe":
			var request struct {
				ExpectedVersion uint64 `json:"expected_version"`
				HighWater       uint64 `json:"high_water"`
			}
			if err := json.NewDecoder(r.Body).Decode(&request); err != nil || request.ExpectedVersion != state.Version || request.HighWater != 3 {
				t.Errorf("durable high-water report mismatch: %+v err=%v", request, err)
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			state = transition(t, state, board.ObserveAgentGeneration{Actor: "board-agent", HighWater: request.HighWater})
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
		default:
			t.Errorf("unexpected route %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer closeServer()
	result, err := c.ObserveAgentGeneration(context.Background(), "ek-ra8d2", 3)
	if err != nil || result.Phase != board.Quarantined || result.AgentHighWater != 3 {
		t.Fatalf("database rollback was not quarantined: %+v %v", result, err)
	}
}
