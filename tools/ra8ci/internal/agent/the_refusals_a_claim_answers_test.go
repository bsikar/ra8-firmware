// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// A claim is the one request an idle agent makes over and over. Everything it
// refuses before spending that request is refused here, so an operator reading
// a refused claim learns which side was at fault: this host's own trust file
// and readings, or the grant the plane answered with.

func TestClaimRefusesAnIncompleteInvocation(t *testing.T) {
	var nilAgent *Agent
	if assigned, err := nilAgent.RunOnce(context.Background()); assigned || !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("a nil agent claimed: %v, %v", assigned, err)
	}
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("a claim was sent without a context: %s", r.URL.Path)
	})
	defer server.Close()
	//nolint:staticcheck // the nil context is the refusal under test.
	if assigned, err := agent.RunOnce(nil); assigned || !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("a claim with no context was sent: %v, %v", assigned, err)
	}
}

// The trust file is re-asked BEFORE the request. A lapsed bundle fails the
// handshake while verifying the SERVER's chain, so the error would otherwise
// name the server and send the operator to the listener's log for a fault
// that is on this host.
func TestClaimRefusesALapsedTrustFileWithoutSpendingARequest(t *testing.T) {
	var claims int
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		claims++
		w.WriteHeader(http.StatusNoContent)
	})
	defer server.Close()
	agent.authorities = func() error { return errors.New("trust bundle expired 2026-09-01") }

	assigned, err := agent.RunOnce(context.Background())
	if assigned || !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("claim = %v, %v", assigned, err)
	}
	if claims != 0 {
		t.Fatalf("a lapsed trust file still spent %d request(s)", claims)
	}
	// The operator has to be able to read WHICH file lapsed, so the
	// underlying reason travels rather than being flattened to the kind.
	if !strings.Contains(err.Error(), "trust bundle expired 2026-09-01") {
		t.Fatalf("the refusal does not name the lapsed bundle: %v", err)
	}
	// A trust file that still verifies is not an obstacle: the claim goes,
	// and an empty poll is answered as no work rather than as a failure.
	agent.authorities = func() error { return nil }
	if assigned, err := agent.RunOnce(context.Background()); assigned || err != nil {
		t.Fatalf("a sound trust file refused the claim: %v, %v", assigned, err)
	}
	if claims != 1 {
		t.Fatalf("claims sent = %d, want 1", claims)
	}
}

// PollWaitMS is the one field of the claim this host chooses, and the
// protocol bounds it at 25s. A misconfigured wait is refused here rather
// than parked on the plane's poll for as long as it asks.
func TestClaimRefusesAWaitTheProtocolWillNotHold(t *testing.T) {
	var claims int
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		claims++
		w.WriteHeader(http.StatusNoContent)
	})
	defer server.Close()
	agent.pollWait = 26 * time.Second

	if assigned, err := agent.RunOnce(context.Background()); assigned || !errors.Is(err, protocol.ErrInvalid) {
		t.Fatalf("claim = %v, %v", assigned, err)
	}
	if claims != 0 {
		t.Fatalf("an out-of-bounds wait still spent %d request(s)", claims)
	}
	// The bound itself is held, not refused.
	agent.pollWait = 25 * time.Second
	if assigned, err := agent.RunOnce(context.Background()); assigned || err != nil {
		t.Fatalf("the bound itself was refused: %v, %v", assigned, err)
	}
	if claims != 1 {
		t.Fatalf("claims sent = %d, want 1", claims)
	}
}

// A grant that does not validate is reported as CLAIMED. The plane has
// already handed the attempt to this agent, so answering false would leave
// the attempt fenced on the plane with no one reporting against it.
func TestAnInvalidGrantIsRefusedButStillClaimed(t *testing.T) {
	grant := testAssignment()
	grant.FencingToken = 0
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/agents/me/claim" {
			t.Errorf("unexpected endpoint %s", r.URL.Path)
		}
		w.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(w).Encode(grant); err != nil {
			t.Error(err)
		}
	})
	defer server.Close()

	assigned, err := agent.RunOnce(context.Background())
	if !assigned {
		t.Fatalf("an invalid grant was reported unclaimed: %v, %v", assigned, err)
	}
	if !errors.Is(err, ErrUnsafeAssignment) || !strings.Contains(err.Error(), "invalid grant") {
		t.Fatalf("refusal = %v", err)
	}
}

// The checkout is verified before the attempt is acknowledged, so an agent
// pointed at a tree that is not the reviewed catalog never tells the plane
// it is about to run one.
func TestAnUnverifiableCheckoutIsRefusedBeforeTheAck(t *testing.T) {
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	grant := testAssignment()
	grant.CatalogSHA256 = definitions.Digest()
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/ack") {
			t.Errorf("an unverifiable checkout was acknowledged to the plane")
		}
		w.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(w).Encode(grant); err != nil {
			t.Error(err)
		}
	})
	defer server.Close()
	// testAgent's root is an empty directory: no catalog, no git.

	assigned, err := agent.RunOnce(context.Background())
	if !assigned {
		t.Fatal("the attempt was claimed, so it must be reported as claimed")
	}
	if !errors.Is(err, ErrUnsafeAssignment) || !strings.Contains(err.Error(), "catalog checkout") {
		t.Fatalf("refusal = %v", err)
	}
}

// A plane that refuses the ack ends the attempt there. The agent does not
// run a task it was never told to start.
func TestAnAckThePlaneRefusesEndsTheAttempt(t *testing.T) {
	root, snapshot := fixtureCheckout(t, "printf 'agent-log\\n'\n")
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	grant := testAssignment()
	grant.CatalogSHA256 = definitions.Digest()
	grant.Source.Commit, grant.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest

	var ran bool
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case strings.HasSuffix(r.URL.Path, "/ack"):
			http.Error(w, "the plane has fenced this attempt", http.StatusConflict)
		case strings.Contains(r.URL.Path, "/logs"), strings.Contains(r.URL.Path, "/result"):
			ran = true
			w.WriteHeader(http.StatusNoContent)
		default:
			w.Header().Set("Content-Type", "application/json")
			if err := json.NewEncoder(w).Encode(grant); err != nil {
				t.Error(err)
			}
		}
	})
	defer server.Close()
	agent.root = root

	assigned, err := agent.RunOnce(context.Background())
	if !assigned || err == nil {
		t.Fatalf("claim = %v, %v", assigned, err)
	}
	if ran {
		t.Fatal("the task ran after the plane refused the ack")
	}
	if !strings.Contains(err.Error(), fmt.Sprint(http.StatusConflict)) {
		t.Fatalf("the refusal does not carry the plane's answer: %v", err)
	}
}
