package boardclient

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func recoveringInProgress(t *testing.T) board.Snapshot {
	t.Helper()
	state := transition(t, activeBoard(t), board.AgentUnavailable{Actor: "board-agent", Reason: "monotonic clock continuity lost"})
	return transition(t, state, board.BeginRecovery{Actor: "operator", PlanID: testPlanID, Reason: "holder died mid-lease"})
}

func recoveryChallenge(state board.Snapshot) store.NeutralChallenge {
	return store.NeutralChallenge{
		ID: testProofID, Nonce: "nonce", BoardID: state.BoardID, Purpose: "recovery",
		Generation: state.Generation, SnapshotVersion: state.Version,
		AgentHighWater: state.AgentHighWater, FixtureRevision: "rev-3",
		ProfileSHA256: "abc", RestorePolicy: "reflash",
		IssuedAt: time.Now().UTC(), ExpiresAt: time.Now().UTC().Add(30 * time.Second),
	}
}

func TestFinishRecoverySubmitsAChallengeBoundReceipt(t *testing.T) {
	state := recoveringInProgress(t)
	ready := transition(t, state, board.CompleteRecovery{Actor: "operator",
		NeutralReceipt: "signed", AgentHighWater: state.AgentHighWater})
	producer := &receiptProducer{receipt: []byte("board-agent-signature")}
	var posted struct {
		ExpectedVersion uint64 `json:"expected_version"`
		ChallengeID     string `json:"challenge_id"`
		Receipt         []byte `json:"receipt"`
	}
	challenges, completions := 0, 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/neutral-challenge":
			challenges++
			jsonResponse(w, http.StatusCreated, recoveryChallenge(state))
		case "/v1/boards/ek-ra8d2/recovery/complete":
			completions++
			if err := json.NewDecoder(r.Body).Decode(&posted); err != nil {
				t.Errorf("completion body did not decode: %v", err)
			}
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": ready, "events": []board.Event{}})
		default:
			t.Errorf("unexpected path %s", r.URL.Path)
		}
	})
	defer closeServer()

	snapshot, err := c.FinishRecovery(context.Background(), "ek-ra8d2", producer)
	if err != nil || challenges != 1 || completions != 1 {
		t.Fatalf("recovery did not finish: challenges=%d completions=%d err=%v", challenges, completions, err)
	}
	if posted.ChallengeID != testProofID || string(posted.Receipt) != "board-agent-signature" || posted.ExpectedVersion != state.Version {
		t.Fatalf("submission did not carry the challenge it signed: %+v", posted)
	}
	if producer.seen.Purpose != "recovery" || producer.seen.Nonce != "nonce" {
		t.Fatalf("the producer signed the wrong challenge: %+v", producer.seen)
	}
	if snapshot.Phase != board.Ready {
		t.Fatalf("phase after finishing: got %s, want ready", snapshot.Phase)
	}
}

// Nothing may be sent without a producer: an operator saying the hardware is
// safe is not evidence that it is.
func TestFinishRecoveryWithoutAProducerSendsNothing(t *testing.T) {
	requests := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		requests++
		jsonResponse(w, http.StatusOK, map[string]any{})
	})
	defer closeServer()

	if _, err := c.FinishRecovery(context.Background(), "ek-ra8d2", nil); !errors.Is(err, ErrNeutralUnavailable) {
		t.Fatalf("got %v, want ErrNeutralUnavailable", err)
	}
	if _, err := c.FinishRecovery(context.Background(), "", &receiptProducer{receipt: []byte("x")}); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("got %v, want ErrInvalidRequest", err)
	}
	if requests != 0 {
		t.Fatalf("a refused completion reached the server %d times", requests)
	}
}

// A board that is not mid-recovery has nothing to finish, and no challenge is
// even asked for.
func TestFinishRecoveryRefusesABoardThatIsNotRecovering(t *testing.T) {
	state := activeBoard(t)
	challenges := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/boards/ek-ra8d2" {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		challenges++
		jsonResponse(w, http.StatusCreated, recoveryChallenge(state))
	})
	defer closeServer()

	if _, err := c.FinishRecovery(context.Background(), "ek-ra8d2", &receiptProducer{receipt: []byte("x")}); !errors.Is(err, ErrNoRecoveryPending) {
		t.Fatalf("got %v, want ErrNoRecoveryPending", err)
	}
	if challenges != 0 {
		t.Fatalf("a challenge was requested %d times for a board that is not recovering", challenges)
	}
}

// A challenge that does not describe this board, this version, or this purpose
// is not the one the server will verify, and it is never signed.
func TestFinishRecoveryRefusesAChallengeThatDoesNotFit(t *testing.T) {
	state := recoveringInProgress(t)
	for name, spoil := range map[string]func(store.NeutralChallenge) store.NeutralChallenge{
		"wrong board":   func(c store.NeutralChallenge) store.NeutralChallenge { c.BoardID = "ek-ra8d1"; return c },
		"wrong purpose": func(c store.NeutralChallenge) store.NeutralChallenge { c.Purpose = "release"; return c },
		"wrong version": func(c store.NeutralChallenge) store.NeutralChallenge { c.SnapshotVersion++; return c },
		"no profile":    func(c store.NeutralChallenge) store.NeutralChallenge { c.ProfileSHA256 = ""; return c },
		"expired": func(c store.NeutralChallenge) store.NeutralChallenge {
			c.ExpiresAt = time.Now().UTC().Add(-time.Second)
			return c
		},
	} {
		producer := &receiptProducer{receipt: []byte("x")}
		completions := 0
		c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/v1/boards/ek-ra8d2":
				jsonResponse(w, http.StatusOK, state)
			case "/v1/boards/ek-ra8d2/neutral-challenge":
				jsonResponse(w, http.StatusCreated, spoil(recoveryChallenge(state)))
			default:
				completions++
				jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
			}
		})
		if _, err := c.FinishRecovery(context.Background(), "ek-ra8d2", producer); !errors.Is(err, ErrInvalidNeutralProof) {
			t.Errorf("%s: got %v, want ErrInvalidNeutralProof", name, err)
		}
		if producer.seen.ID != "" || completions != 0 {
			t.Errorf("%s: an unusable challenge was signed or submitted", name)
		}
		closeServer()
	}
}
