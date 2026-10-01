// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/actions/scaleset"
	"github.com/google/uuid"
)

// A replay is not a second chance to be lenient. Work saved before an
// acknowledgement is carried out under the same admission and the same
// handler as work taken live, and a failure on either stops the listener
// before it polls for anything new.

// saved is one inbox entry the replay will find waiting.
func saved(id int) Message {
	return Message{
		ScaleSetID: 42,
		SessionID:  "6f9619ff-8b86-d011-b42d-00cf4fc964ff",
		MessageID:  id,
		Statistics: Statistics{Assigned: 1},
		Available: []Job{{
			Kind: "JobAvailable", RunnerRequestID: int64(90 + id),
			Repository: "ra8-firmware", Owner: "bsikar",
		}},
	}
}

// replaying builds a controller over waiting inbox entries, with an
// admission and a handler the caller chooses.
func replaying(t *testing.T, inbox *scriptedInbox, admission Admission, handler Handler) (*Controller, *idleClient) {
	t.Helper()
	client := &idleClient{session: scaleset.RunnerScaleSetSession{
		SessionID:  uuid.New(),
		Statistics: &scaleset.RunnerScaleSetStatistic{TotalIdleRunners: 1},
	}}
	built, err := NewController(client, inbox, handler, admission, 42, 2, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	return built, client
}

// A job the admission refuses during replay stops the listener there. The
// message stays in the inbox, the handler never sees it, and no poll is
// spent: a listener that polled on would be acquiring new work while
// holding work it has been told it may not run.
func TestAJobRefusedDuringReplayStopsTheListenerBeforeItPolls(t *testing.T) {
	inbox := &scriptedInbox{pending: []Message{saved(1), saved(2)}}
	handler := &countingHandler{}
	controller, client := replaying(t, inbox, testAdmission{err: errors.New("untrusted workflow")}, handler)

	err := controller.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "github job admission") {
		t.Fatalf("Run = %v", err)
	}
	if !strings.Contains(err.Error(), "untrusted workflow") {
		t.Fatalf("the refusal lost its reason: %v", err)
	}
	if handler.processed != 0 {
		t.Fatalf("a refused message was processed %d times", handler.processed)
	}
	if client.gets != 0 {
		t.Fatalf("the listener polled %d times while holding refused work", client.gets)
	}
	if len(inbox.pending) != 2 || inbox.processed != 0 {
		t.Fatalf("pending=%d processed=%d after a refusal", len(inbox.pending), inbox.processed)
	}
}

// A handler that fails during replay stops the listener the same way, and
// the message it failed on is still waiting afterwards. That is what makes
// the replay worth having: the work survives the process that dropped it.
func TestAHandlerFailureDuringReplayLeavesTheWorkWaiting(t *testing.T) {
	inbox := &scriptedInbox{pending: []Message{saved(1)}}
	handler := &countingHandler{processErr: errors.New("proxmox unavailable")}
	controller, client := replaying(t, inbox, testAdmission{}, handler)

	err := controller.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "process github message") {
		t.Fatalf("Run = %v", err)
	}
	if !strings.Contains(err.Error(), "proxmox unavailable") {
		t.Fatalf("the failure lost its reason: %v", err)
	}
	if handler.processed != 1 {
		t.Fatalf("the waiting message was handed over %d times", handler.processed)
	}
	if client.gets != 0 {
		t.Fatalf("the listener polled %d times before finishing its replay", client.gets)
	}
	if len(inbox.pending) != 1 || inbox.processed != 0 {
		t.Fatalf("pending=%d processed=%d after a handler failure", len(inbox.pending), inbox.processed)
	}

	// The same replay, once the handler recovers, drains the inbox and
	// only then does the listener start polling.
	handler.processErr = nil
	if err := controller.replay(context.Background()); err != nil {
		t.Fatalf("the recovered replay = %v", err)
	}
	if len(inbox.pending) != 0 || inbox.processed != 1 {
		t.Fatalf("pending=%d processed=%d after recovery", len(inbox.pending), inbox.processed)
	}
	if handler.processed != 2 {
		t.Fatalf("the recovered replay handed over %d messages", handler.processed)
	}
}

// An entry belonging to another scale set is a sign the inbox is not the
// one this listener owns. It is refused by name rather than run or quietly
// skipped, since either would have this listener acting on a scale set it
// was never given.
func TestAForeignScaleSetInTheInboxIsNamedRatherThanRun(t *testing.T) {
	foreign := saved(1)
	foreign.ScaleSetID = 43
	inbox := &scriptedInbox{pending: []Message{foreign}}
	handler := &countingHandler{}
	controller, client := replaying(t, inbox, testAdmission{}, handler)

	err := controller.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "foreign scale set 43") {
		t.Fatalf("Run = %v", err)
	}
	if handler.processed != 0 || client.gets != 0 {
		t.Fatalf("processed=%d gets=%d over a foreign inbox", handler.processed, client.gets)
	}
	if len(inbox.pending) != 1 {
		t.Fatalf("the foreign entry was consumed: pending=%d", len(inbox.pending))
	}
}

// An inbox that cannot be read at all is reported as the read it was, not
// as an empty replay. An empty replay would let the listener poll on as if
// nothing had been left behind.
func TestAnUnreadableInboxIsNotAnEmptyReplay(t *testing.T) {
	inbox := &scriptedInbox{pendErr: errors.New("connection refused")}
	handler := &countingHandler{}
	controller, client := replaying(t, inbox, testAdmission{}, handler)

	err := controller.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "read github inbox") {
		t.Fatalf("Run = %v", err)
	}
	if !strings.Contains(err.Error(), "connection refused") {
		t.Fatalf("the read failure lost its reason: %v", err)
	}
	if client.gets != 0 || handler.processed != 0 || handler.reconciled != 0 {
		t.Fatalf("gets=%d processed=%d reconciled=%d over an unreadable inbox",
			client.gets, handler.processed, handler.reconciled)
	}
}
