package boardclient

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const testRecoveryPlanID = "01996f90-3415-7cfe-8ff1-600058131ab1"

func planned(challenge store.NeutralChallenge, planID string) store.NeutralChallenge {
	challenge.RecoveryPlanID = planID
	return challenge
}

// The plan is the field naming the reviewed hardware sequence, so a recovery
// challenge without one is not a challenge for a reviewed recovery.
func TestARecoveryChallengeMustNameItsPlan(t *testing.T) {
	base := store.NeutralChallenge{Purpose: "recovery"}
	for name, challenge := range map[string]store.NeutralChallenge{
		"no plan":         base,
		"blank plan":      planned(base, "   "),
		"not an ID":       planned(base, "plan-3"),
		"lease ID shaped": planned(base, testRecoveryPlanID[:len(testRecoveryPlanID)-1]),
	} {
		if checkChallengeContext(challenge) {
			t.Errorf("%s: a recovery challenge was accepted without a reviewed plan", name)
		}
	}
	if !checkChallengeContext(planned(base, testRecoveryPlanID)) {
		t.Fatal("a recovery challenge naming its plan was refused")
	}
}

// A release ends a lease, not a hardware sequence, and the server only stamps a
// plan onto the recovery arm. One arriving on a release is a challenge issued
// under a context this call is not making.
func TestAReleaseChallengeCarriesNoPlan(t *testing.T) {
	base := store.NeutralChallenge{Purpose: "release"}
	if !checkChallengeContext(base) {
		t.Fatal("an ordinary release challenge was refused")
	}
	if checkChallengeContext(planned(base, testRecoveryPlanID)) {
		t.Fatal("a release challenge carrying a recovery plan was accepted")
	}
}

// Both doors name their purpose before this is reached, so an unknown purpose
// can only be a challenge neither of them asked for.
func TestAnUnknownPurposeHasNoContextToCheck(t *testing.T) {
	for _, purpose := range []string{"", "Release", "recover", "neutral", "hil"} {
		if checkChallengeContext(store.NeutralChallenge{Purpose: purpose,
			RecoveryPlanID: testRecoveryPlanID}) {
			t.Errorf("purpose %q was accepted", purpose)
		}
	}
}

// Free's end of the rule: a release challenge arriving with a recovery plan on
// it is refused before the board agent is asked to sign anything.
func TestFreeRefusesAReleaseChallengeCarryingARecoveryPlan(t *testing.T) {
	state := activeBoard(t)
	token := LeaseToken{BoardID: state.BoardID, RequestID: state.Lease.WaiterID,
		LeaseID: state.Lease.ID, Generation: state.Lease.Generation,
		ExpiresAt: state.Lease.ExpiresAt, Version: state.Version}
	producer := &receiptProducer{receipt: []byte("x")}
	releases := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/neutral-challenge":
			jsonResponse(w, http.StatusCreated, store.NeutralChallenge{
				ID: testProofID, Nonce: "nonce", BoardID: state.BoardID, Purpose: "release",
				LeaseID: token.LeaseID, Generation: token.Generation,
				SnapshotVersion: state.Version, FixtureRevision: "rev-3", ProfileSHA256: "abc",
				RecoveryPlanID: testRecoveryPlanID,
				IssuedAt:       time.Now().UTC(), ExpiresAt: time.Now().UTC().Add(30 * time.Second),
			})
		default:
			releases++
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
		}
	})
	defer closeServer()

	if _, err := c.Free(context.Background(), token, producer); !errors.Is(err, ErrInvalidNeutralProof) {
		t.Fatalf("got %v, want ErrInvalidNeutralProof", err)
	}
	if producer.seen.ID != "" || releases != 0 {
		t.Fatal("a release challenge carrying a recovery plan was signed or submitted")
	}
}
