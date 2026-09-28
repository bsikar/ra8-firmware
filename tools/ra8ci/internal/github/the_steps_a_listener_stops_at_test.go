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

// The controller never runs workflow code; what it does is decide, at every
// step, whether it is safe to carry on. Each failure below stops the
// listener so a supervisor restarts it, rather than letting it poll on with
// work unrecorded. controller_test.go pins the happy replay and the
// admission refusal; this pins what happens when a step underneath fails.

// scriptedClient answers a fixed queue of responses and can fail at any of
// the three calls the controller makes on it.
type scriptedClient struct {
	session    scaleset.RunnerScaleSetSession
	queue      []*scaleset.RunnerScaleSetMessage
	getErr     error
	deleteErr  error
	acquireErr error
	gets       int
	deleted    []int
	acquired   [][]int64
}

func (s *scriptedClient) GetMessage(context.Context, int, int) (*scaleset.RunnerScaleSetMessage, error) {
	s.gets++
	if s.getErr != nil {
		return nil, s.getErr
	}
	if len(s.queue) == 0 {
		return nil, nil
	}
	next := s.queue[0]
	s.queue = s.queue[1:]
	return next, nil
}

func (s *scriptedClient) DeleteMessage(_ context.Context, id int) error {
	if s.deleteErr != nil {
		return s.deleteErr
	}
	s.deleted = append(s.deleted, id)
	return nil
}

func (s *scriptedClient) AcquireJobs(_ context.Context, ids []int64) ([]int64, error) {
	if s.acquireErr != nil {
		return nil, s.acquireErr
	}
	s.acquired = append(s.acquired, append([]int64(nil), ids...))
	return ids, nil
}

func (s *scriptedClient) Session() scaleset.RunnerScaleSetSession { return s.session }

// scriptedInbox is a replay inbox that can fail on either side of the replay.
type scriptedInbox struct {
	pending   []Message
	pendErr   error
	markErr   error
	saveErr   error
	saves     int
	processed int
}

func (s *scriptedInbox) Save(_ context.Context, msg Message) error {
	if s.saveErr != nil {
		return s.saveErr
	}
	s.saves++
	s.pending = append(s.pending, msg)
	return nil
}

func (s *scriptedInbox) Pending(context.Context, int) ([]Message, error) {
	if s.pendErr != nil {
		return nil, s.pendErr
	}
	return append([]Message(nil), s.pending...), nil
}

func (s *scriptedInbox) MarkProcessed(_ context.Context, msg Message) error {
	if s.markErr != nil {
		return s.markErr
	}
	s.processed++
	for i, entry := range s.pending {
		if entry.MessageID == msg.MessageID {
			s.pending = append(s.pending[:i], s.pending[i+1:]...)
			break
		}
	}
	return nil
}

// countingHandler records what it was asked and what the caller's deadline
// looked like when it was asked.
type countingHandler struct {
	processed    int
	reconciled   int
	processErr   error
	reconcileErr error
	sawCancelled bool
}

func (h *countingHandler) Process(ctx context.Context, _ Message) error {
	h.processed++
	h.sawCancelled = ctx.Err() != nil
	return h.processErr
}

func (h *countingHandler) Reconcile(ctx context.Context, _ Statistics) error {
	h.reconciled++
	h.sawCancelled = ctx.Err() != nil
	return h.reconcileErr
}

// listening builds a controller over a scripted client and inbox.
func listening(t *testing.T, client *scriptedClient, inbox *scriptedInbox, handler Handler) *Controller {
	t.Helper()
	if client.session.SessionID == uuid.Nil {
		client.session = scaleset.RunnerScaleSetSession{
			SessionID:  uuid.New(),
			Statistics: &scaleset.RunnerScaleSetStatistic{TotalIdleRunners: 1},
		}
	}
	controller, err := NewController(client, inbox, handler, testAdmission{}, 42, 2, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	return controller
}

// oneAvailable is a response the controller can carry all the way through.
func oneAvailable(id int) *scaleset.RunnerScaleSetMessage {
	return &scaleset.RunnerScaleSetMessage{
		MessageID:  id,
		Statistics: &scaleset.RunnerScaleSetStatistic{TotalAssignedJobs: 1},
		JobAvailableMessages: []*scaleset.JobAvailable{{
			JobMessageBase: scaleset.JobMessageBase{
				JobMessageType:  scaleset.JobMessageType{MessageType: scaleset.MessageTypeJobAvailable},
				RunnerRequestID: 91, RepositoryName: "ra8-firmware", OwnerName: "bsikar",
			},
		}},
	}
}

// A session with no capacity snapshot is refused before the inbox is even
// read: without it there is nothing to reconcile against.
func TestAListenerWithNoCapacitySnapshotStopsBeforeReadingTheInbox(t *testing.T) {
	client := &scriptedClient{session: scaleset.RunnerScaleSetSession{SessionID: uuid.New()}}
	inbox := &scriptedInbox{pendErr: errors.New("the inbox was read")}

	controller := listening(t, client, inbox, &countingHandler{})
	err := controller.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "lacks statistics") {
		t.Fatalf("err = %v", err)
	}
	if client.gets != 0 {
		t.Fatal("the scale set was polled without a capacity snapshot")
	}
}

// Replay comes first, and every way it can fail stops the listener rather
// than letting new messages pile on top of unfinished ones.
func TestAReplayThatCannotFinishStopsTheListener(t *testing.T) {
	unreadable := &scriptedInbox{pendErr: errors.New("database unavailable")}
	client := &scriptedClient{}
	controller := listening(t, client, unreadable, &countingHandler{})
	if err := controller.Run(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "read github inbox") {
		t.Fatalf("an unreadable inbox: %v", err)
	}
	if client.gets != 0 {
		t.Fatal("the scale set was polled before the inbox was replayed")
	}

	// A message belonging to another scale set is the one thing replay
	// must never process: it was written by a different controller.
	foreign := &scriptedInbox{pending: []Message{{ScaleSetID: 43, MessageID: 5}}}
	handler := &countingHandler{}
	controller = listening(t, &scriptedClient{}, foreign, handler)
	err := controller.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "foreign scale set 43") {
		t.Fatalf("a foreign message: %v", err)
	}
	if handler.processed != 0 {
		t.Fatal("a foreign message was processed")
	}
}

// Reconciling is how desired capacity reaches the scaler, so a handler that
// cannot reconcile stops the listener before a single message is polled.
func TestAHandlerThatCannotReconcileStopsTheListenerBeforePolling(t *testing.T) {
	client := &scriptedClient{}
	handler := &countingHandler{reconcileErr: errors.New("scaler unavailable")}

	controller := listening(t, client, &scriptedInbox{}, handler)
	err := controller.Run(context.Background())
	if err == nil || !strings.Contains(err.Error(), "reconcile github scale set") {
		t.Fatalf("err = %v", err)
	}
	if handler.reconciled != 1 || client.gets != 0 {
		t.Fatalf("reconciled=%d polled=%d", handler.reconciled, client.gets)
	}
}

// Each remote step the controller depends on stops it by name when it
// fails, and stops it at the step that failed rather than further on.
func TestEachRemoteStepStopsTheListenerByName(t *testing.T) {
	polling := &scriptedClient{getErr: errors.New("connection reset")}
	handler := &countingHandler{}
	controller := listening(t, polling, &scriptedInbox{}, handler)
	if err := controller.Run(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "poll github scale set") {
		t.Fatalf("a failed poll: %v", err)
	}
	if handler.processed != 0 {
		t.Fatal("a message was processed after a failed poll")
	}

	// Acknowledgement fails after the message is saved, so the message
	// stays pending and the handler is not run: it will be replayed.
	unacknowledged := &scriptedClient{
		queue: []*scaleset.RunnerScaleSetMessage{oneAvailable(7)}, deleteErr: errors.New("gateway timeout"),
	}
	inbox := &scriptedInbox{}
	handler = &countingHandler{}
	controller = listening(t, unacknowledged, inbox, handler)
	if err := controller.Run(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "acknowledge persisted github message") {
		t.Fatalf("a failed acknowledgement: %v", err)
	}
	if handler.processed != 0 || inbox.saves != 1 || len(inbox.pending) != 1 {
		t.Fatalf("processed=%d saves=%d pending=%d", handler.processed, inbox.saves, len(inbox.pending))
	}

	// Acquiring is the first thing processing does, so a failure there
	// leaves the handler unasked and the message still pending.
	unacquirable := &scriptedClient{
		queue: []*scaleset.RunnerScaleSetMessage{oneAvailable(8)}, acquireErr: errors.New("job already acquired"),
	}
	inbox = &scriptedInbox{}
	handler = &countingHandler{}
	controller = listening(t, unacquirable, inbox, handler)
	if err := controller.Run(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "acquire github jobs") {
		t.Fatalf("a failed acquire: %v", err)
	}
	if handler.processed != 0 || inbox.processed != 0 {
		t.Fatalf("processed=%d marked=%d", handler.processed, inbox.processed)
	}

	// The handler ran and the inbox could not be told: the message stays
	// pending, which is the safe direction, since Process is idempotent.
	unmarkable := &scriptedClient{queue: []*scaleset.RunnerScaleSetMessage{oneAvailable(9)}}
	inbox = &scriptedInbox{markErr: errors.New("database unavailable")}
	handler = &countingHandler{}
	controller = listening(t, unmarkable, inbox, handler)
	if err := controller.Run(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "mark github message processed") {
		t.Fatalf("a failed mark: %v", err)
	}
	if handler.processed != 1 || len(inbox.pending) != 1 {
		t.Fatalf("processed=%d pending=%d", handler.processed, len(inbox.pending))
	}
}

// A caller that has already given up is honoured before anything is polled.
func TestACallerThatHasGivenUpStopsTheListenerAtOnce(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	client := &scriptedClient{queue: []*scaleset.RunnerScaleSetMessage{oneAvailable(7)}}
	controller := listening(t, client, &scriptedInbox{}, &countingHandler{})

	err := controller.Run(ctx)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("err = %v", err)
	}
	if client.gets != 0 {
		t.Fatal("the scale set was polled after the caller gave up")
	}
}

// Processing is deliberately detached from the caller's cancellation and
// reconciling is deliberately not. A message that has already been
// acknowledged must still be recorded even as the listener shuts down,
// while a reconcile is only ever advice about capacity and can be dropped.
func TestProcessingOutlivesACancelledCallerAndReconcilingDoesNot(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	inbox := &scriptedInbox{}
	handler := &countingHandler{}
	controller := listening(t, &scriptedClient{}, inbox, handler)

	entry := Message{ScaleSetID: 42, MessageID: 7}
	if err := controller.process(ctx, entry); err != nil {
		t.Fatalf("processing was abandoned with the caller: %v", err)
	}
	if handler.processed != 1 || handler.sawCancelled {
		t.Fatalf("the handler saw a cancelled caller: processed=%d cancelled=%v",
			handler.processed, handler.sawCancelled)
	}
	if inbox.processed != 1 {
		t.Fatal("an acknowledged message was left unrecorded")
	}

	if err := controller.reconcile(ctx, Statistics{Idle: 1}); err != nil {
		t.Fatalf("reconcile = %v", err)
	}
	if handler.reconciled != 1 || !handler.sawCancelled {
		t.Fatalf("reconciling did not carry the caller's cancellation: %v", handler.sawCancelled)
	}
}
