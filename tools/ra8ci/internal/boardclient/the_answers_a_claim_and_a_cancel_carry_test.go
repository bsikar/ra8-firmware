// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// A cancellation, a liveness report and a HIL claim all answer about one
// board and one lease. Each of them is refused here when what came back is
// about something else, because acting on it would touch hardware somebody
// else is holding.

// answering serves a fixed board document on every route and counts the
// commands that were submitted against it.
func answering(t *testing.T, state board.Snapshot, document any) (*Client, *int, func()) {
	t.Helper()
	commands := 0
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.URL.Path == "/v1/boards/ek-ra8d2" && document != nil:
			jsonResponse(w, http.StatusOK, document)
		case r.URL.Path == "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case r.URL.Path == "/v1/boards/ek-ra8d2/liveness":
			jsonResponse(w, http.StatusOK, document)
		default:
			commands++
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
		}
	})
	return client, &commands, done
}

// A ticket this client cannot name is refused before a request is spent,
// and a ticket the board has already granted is not cancelled: the holder
// is running on that lease, and cancelling it out from under them is the
// one thing a late cancel must not do.
func TestCancelRefusesATicketItCannotAct(t *testing.T) {
	granted := activeBoard(t)
	token := testToken(granted)
	client, commands, done := answering(t, granted, nil)
	defer done()

	for name, ticket := range map[string]Ticket{
		"a board ID that is not one":   {BoardID: "not a board id", RequestID: testRequestID, LeaseID: testLeaseID},
		"a request ID that is not one": {BoardID: "ek-ra8d2", RequestID: "request-1", LeaseID: testLeaseID},
		"a lease ID that is not one":   {BoardID: "ek-ra8d2", RequestID: testRequestID, LeaseID: "lease-1"},
		"no request ID at all":         {BoardID: "ek-ra8d2", LeaseID: testLeaseID},
	} {
		if err := client.Cancel(context.Background(), ticket); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
	}

	held := Ticket{BoardID: token.BoardID, RequestID: token.RequestID, LeaseID: token.LeaseID}
	if err := client.Cancel(context.Background(), held); !errors.Is(err, ErrAlreadyGranted) {
		t.Fatalf("cancelling a granted lease = %v", err)
	}
	if *commands != 0 {
		t.Fatalf("a cancellation was submitted for a lease already granted (%d commands)", *commands)
	}
}

// A ticket the board no longer carries is already cancelled as far as the
// caller is concerned, so the answer is success with nothing submitted.
// An unreachable server, by contrast, is not evidence the waiter is gone.
func TestCancelIsSatisfiedByAWaiterAlreadyGone(t *testing.T) {
	empty, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	client, commands, done := answering(t, empty, nil)
	gone := Ticket{BoardID: "ek-ra8d2", RequestID: testRequestID, LeaseID: testLeaseID}
	if err := client.Cancel(context.Background(), gone); err != nil {
		t.Fatalf("cancelling a waiter already gone = %v", err)
	}
	if *commands != 0 {
		t.Fatalf("a cancellation was submitted for a waiter nobody holds (%d commands)", *commands)
	}
	done()

	if err := client.Cancel(context.Background(), gone); err == nil {
		t.Fatal("an unreachable server was read as a waiter already cancelled")
	}
}

// A liveness report is about one board and one lease. A report naming
// another board, or a lease this board does not record, is refused rather
// than handed on as the holder's health.
func TestLivenessRefusesAReportAboutAnotherLease(t *testing.T) {
	state := activeBoard(t)

	if _, _, err := (&Client{}).Liveness(context.Background(), "not a board id"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("a board ID that is not one = %v", err)
	}

	other := activeBoard(t)
	other.BoardID = "ek-ra8m1"
	for name, document := range map[string]any{
		"a snapshot about another board": map[string]any{"snapshot": other},
		"a snapshot that is not valid":   map[string]any{"snapshot": board.Snapshot{BoardID: "ek-ra8d2"}},
		"liveness for a lease this board does not record": map[string]any{
			"snapshot": state,
			"liveness": map[string]any{"held": true, "lease_id": testProofID, "healthy": true},
		},
	} {
		client, _, done := answering(t, state, document)
		if _, _, err := client.Liveness(context.Background(), "ek-ra8d2"); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
		done()
	}
}

// An unreachable server is handed back as the transport failure it is. A
// caller that read it as "not held" would free a board somebody is using.
func TestLivenessHandsBackAnUnreachableServer(t *testing.T) {
	client, _, done := answering(t, activeBoard(t), nil)
	done()

	if _, reported, err := client.Liveness(context.Background(), "ek-ra8d2"); err == nil || reported.Held {
		t.Fatalf("an unreachable server was read as board liveness: held=%v err=%v", reported.Held, err)
	}
}

// A claim describes the host that would run the work. A host that cannot
// be believed is refused before a board is committed to it, since the
// server sizes and schedules the attempt from exactly these numbers.
func TestClaimNextHILAttemptRefusesAHostItCannotBelieve(t *testing.T) {
	client, commands, done := answering(t, activeBoard(t), nil)
	defer done()
	facts := json.RawMessage(`{"os":"linux"}`)

	for name, ask := range map[string]struct {
		boardID  string
		leaseID  string
		host     string
		cores    int
		ram      int64
		load     float64
		hostFcts json.RawMessage
	}{
		"a board ID that is not one":   {"not a board id", testLeaseID, "runner-1", 4, 1 << 30, 0.5, facts},
		"a lease ID that is not one":   {"ek-ra8d2", "lease-1", "runner-1", 4, 1 << 30, 0.5, facts},
		"no host":                      {"ek-ra8d2", testLeaseID, "", 4, 1 << 30, 0.5, facts},
		"no cores":                     {"ek-ra8d2", testLeaseID, "runner-1", 0, 1 << 30, 0.5, facts},
		"no memory":                    {"ek-ra8d2", testLeaseID, "runner-1", 4, 0, 0.5, facts},
		"a load below zero":            {"ek-ra8d2", testLeaseID, "runner-1", 4, 1 << 30, -1, facts},
		"no host facts":                {"ek-ra8d2", testLeaseID, "runner-1", 4, 1 << 30, 0.5, nil},
		"host facts that are not JSON": {"ek-ra8d2", testLeaseID, "runner-1", 4, 1 << 30, 0.5, json.RawMessage(`{`)},
		"host facts that are not an object": {"ek-ra8d2", testLeaseID, "runner-1", 4, 1 << 30, 0.5,
			json.RawMessage(`["linux"]`)},
		"host facts that are null": {"ek-ra8d2", testLeaseID, "runner-1", 4, 1 << 30, 0.5, json.RawMessage(`null`)},
	} {
		assignment, err := client.ClaimNextHILAttempt(context.Background(), ask.boardID, ask.leaseID,
			ask.host, ask.cores, ask.ram, ask.load, ask.hostFcts)
		if !errors.Is(err, ErrInvalidRequest) || assignment != nil {
			t.Fatalf("%s = %v (assignment %v)", name, err, assignment)
		}
	}
	if *commands != 0 {
		t.Fatalf("a claim was sent for a host that cannot be believed (%d commands)", *commands)
	}
}

// Nothing to claim is not a failure, and it is not an assignment either.
func TestClaimNextHILAttemptAnswersAnEmptyQueue(t *testing.T) {
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusOK, map[string]any{"assignment": nil})
	})
	defer done()

	assignment, err := client.ClaimNextHILAttempt(context.Background(), "ek-ra8d2", testLeaseID,
		"runner-1", 4, 1<<30, 0.5, json.RawMessage(`{"os":"linux"}`))
	if err != nil || assignment != nil {
		t.Fatalf("an empty queue answered %v, err=%v", assignment, err)
	}
}

// An assignment that is not catalog-bound is refused rather than run. The
// board agent flashes whatever this describes, so an attempt naming another
// board, another scope, or a digest that is not one is not a thing to run.
func TestClaimNextHILAttemptRefusesAnAssignmentThatIsNotCatalogBound(t *testing.T) {
	task := observedTask()
	for name, assignment := range map[string]any{
		"an attempt ID that is not one": map[string]any{
			"attempt": map[string]any{"id": "attempt-1", "task_id": testRequestID, "state": "running"},
			"task":    task,
		},
		"an attempt that is not running": map[string]any{
			"attempt": map[string]any{"id": testRequestID, "task_id": testLeaseID, "state": "queued"},
			"task":    task,
		},
		"an assignment with no task at all": map[string]any{
			"attempt": map[string]any{"id": testRequestID, "task_id": testLeaseID, "state": "running"},
		},
	} {
		client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
			jsonResponse(w, http.StatusOK, map[string]any{"assignment": assignment})
		})
		got, err := client.ClaimNextHILAttempt(context.Background(), "ek-ra8d2", testLeaseID,
			"runner-1", 4, 1<<30, 0.5, json.RawMessage(`{"os":"linux"}`))
		if err == nil || got != nil {
			t.Fatalf("%s was accepted as an assignment", name)
		}
		done()
	}
}

// A claim the server would not answer is handed back as it happened, not
// as an empty queue a caller would poll past.
func TestClaimNextHILAttemptHandsBackARefusedClaim(t *testing.T) {
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusServiceUnavailable, map[string]any{
			"code": "unavailable", "detail": "scheduler is restarting", "retryable": true,
		})
	})
	defer done()

	if _, err := client.ClaimNextHILAttempt(context.Background(), "ek-ra8d2", testLeaseID,
		"runner-1", 4, 1<<30, 0.5, json.RawMessage(`{"os":"linux"}`)); err == nil {
		t.Fatal("a refused claim was read as an empty queue")
	}
}
