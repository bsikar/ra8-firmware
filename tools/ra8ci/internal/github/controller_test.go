package github

import (
	"context"
	"errors"
	"testing"
	"time"
)

type testHandler struct {
	processed  int
	reconciled int
	err        error
}

func (h *testHandler) Process(_ context.Context, _ Message) error {
	h.processed++
	return h.err
}

func (h *testHandler) Reconcile(_ context.Context, _ Statistics) error {
	h.reconciled++
	return nil
}

type testAdmission struct{ err error }

func (a testAdmission) Allow(_ context.Context, _ Job) error { return a.err }

func TestControllerReplaysAfterHandlerFailureFollowingAck(t *testing.T) {
	client := testClient()
	client.once = true
	inbox := &fakeInbox{}
	handler := &testHandler{err: errors.New("proxmox unavailable")}
	controller, err := NewController(client, inbox, handler, testAdmission{}, 42, 2, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if err := controller.Run(context.Background()); err == nil {
		t.Fatal("handler failure swallowed")
	}
	if len(client.deleted) != 1 || len(inbox.pending) != 1 {
		t.Fatalf("acked=%v pending=%d", client.deleted, len(inbox.pending))
	}
	if len(client.acquired) != 1 || len(client.acquired[0]) != 1 || client.acquired[0][0] != 91 {
		t.Fatalf("acquired=%v", client.acquired)
	}
	handler.err = nil
	if err := controller.replay(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(inbox.pending) != 0 || handler.processed != 2 {
		t.Fatalf("pending=%d processed=%d", len(inbox.pending), handler.processed)
	}
}

func TestControllerRejectsUntrustedJobBeforeAckAndAcquire(t *testing.T) {
	client := testClient()
	inbox := &fakeInbox{}
	controller, err := NewController(client, inbox, &testHandler{}, testAdmission{err: errors.New("untrusted workflow")}, 42, 2, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if err := controller.Run(context.Background()); err == nil {
		t.Fatal("untrusted job accepted")
	}
	if len(client.deleted) != 0 || len(client.acquired) != 0 {
		t.Fatalf("job acknowledged or acquired: deleted=%v acquired=%v", client.deleted, client.acquired)
	}
}

func TestControllerBoundsEmptyPollRateAndHonorsCancellation(t *testing.T) {
	client := testClient()
	client.once = true
	controller, err := NewController(client, &fakeInbox{}, &testHandler{}, testAdmission{}, 42, 2, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancel()
	err = controller.Run(ctx)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("idle controller exit = %v, want context deadline", err)
	}
	if client.gets != 1 {
		t.Fatalf("empty polls were not rate-limited: got %d upstream polls", client.gets)
	}
}

func TestControllerRequiresAdmissionAndTimeout(t *testing.T) {
	if _, err := NewController(testClient(), &fakeInbox{}, &testHandler{}, nil, 42, 1, time.Second); err == nil {
		t.Fatal("nil admission accepted")
	}
	if _, err := NewController(testClient(), &fakeInbox{}, &testHandler{}, testAdmission{}, 42, 1, 0); err == nil {
		t.Fatal("unbounded callback accepted")
	}
}

func TestNewControllerSessionAcceptsPrecomposedHandler(t *testing.T) {
	closed := false
	session := &Session{Client: testClient(), close: func(context.Context) error { closed = true; return nil }, scaleSetID: 42}
	controller, err := NewController(session.Client, &fakeInbox{}, &testHandler{}, testAdmission{}, 42, 1, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	bound, err := NewControllerSession(session, controller)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_ = bound.Run(ctx)
	if !closed {
		t.Fatal("controller session did not close the supplied GitHub session")
	}
}

func TestNewControllerSessionRejectsIncompleteSession(t *testing.T) {
	controller, err := NewController(testClient(), &fakeInbox{}, &testHandler{}, testAdmission{}, 42, 1, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := NewControllerSession(nil, controller); err == nil {
		t.Fatal("nil GitHub session accepted")
	}
	if _, err := NewControllerSession(&Session{Client: testClient()}, controller); err == nil {
		t.Fatal("session without close function accepted")
	}
	if _, err := NewControllerSession(&Session{close: func(context.Context) error { return nil }}, controller); err == nil {
		t.Fatal("session without client accepted")
	}
	if _, err := NewControllerSession(&Session{Client: testClient(), close: func(context.Context) error { return nil }, scaleSetID: 43}, controller); err == nil {
		t.Fatal("controller bound to a different scale set accepted")
	}
}

func TestComposeControllerSessionBuildsHandlerFromExactGitHubSession(t *testing.T) {
	closed := false
	session := &Session{Client: testClient(), close: func(context.Context) error { closed = true; return nil }, scaleSetID: 42}
	var received *Session
	bound, err := ComposeControllerSession(session, &fakeInbox{}, testAdmission{}, 2, time.Second,
		func(got *Session) (Handler, error) { received = got; return &testHandler{}, nil })
	if err != nil {
		t.Fatal(err)
	}
	if received != session {
		t.Fatal("handler factory received a different GitHub session")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_ = bound.Run(ctx)
	if !closed {
		t.Fatal("composed controller did not close its GitHub session")
	}
}

func TestComposeControllerSessionClosesAfterCompositionFailure(t *testing.T) {
	tests := []struct {
		name    string
		timeout time.Duration
		factory HandlerFactory
	}{
		{"handler failure", time.Second, func(*Session) (Handler, error) { return nil, errors.New("construction failed") }},
		{"controller validation failure", 0, func(*Session) (Handler, error) { return &testHandler{}, nil }},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			closed := false
			session := &Session{Client: testClient(), close: func(context.Context) error { closed = true; return nil }, scaleSetID: 42}
			if _, err := ComposeControllerSession(session, &fakeInbox{}, testAdmission{}, 2, tt.timeout, tt.factory); err == nil {
				t.Fatal("invalid controller composition succeeded")
			}
			if !closed {
				t.Fatal("failed composition leaked the GitHub session")
			}
		})
	}
}
