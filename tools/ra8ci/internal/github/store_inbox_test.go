package github

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type fakeInboxStore struct {
	row       store.GitHubMessage
	saved     bool
	processed bool
	filter    string
}

func (f *fakeInboxStore) SaveGitHubMessage(_ context.Context, set, session, id string, payload json.RawMessage) error {
	f.row = store.GitHubMessage{ScaleSetID: set, SessionID: session, MessageID: id, Payload: payload}
	f.saved = true
	return nil
}

func (f *fakeInboxStore) MarkGitHubMessageProcessed(_ context.Context, set, session, id string) error {
	if f.row.ScaleSetID != set || f.row.SessionID != session || f.row.MessageID != id {
		return store.ErrNotFound
	}
	f.processed = true
	return nil
}

func (f *fakeInboxStore) ListPendingGitHubMessagesForScaleSet(_ context.Context, set string, _ int) ([]store.GitHubMessage, error) {
	f.filter = set
	if f.processed || !f.saved {
		return nil, nil
	}
	return []store.GitHubMessage{f.row}, nil
}

func TestStoreInboxRoundTripAndScaleSetFilter(t *testing.T) {
	backend := &fakeInboxStore{}
	inbox, err := NewStoreInbox(backend, 42)
	if err != nil {
		t.Fatal(err)
	}
	message := Message{ScaleSetID: 42, SessionID: "session", MessageID: 7, Statistics: Statistics{Assigned: 1}}
	if err := inbox.Save(context.Background(), message); err != nil {
		t.Fatal(err)
	}
	pending, err := inbox.Pending(context.Background(), 10)
	if err != nil {
		t.Fatal(err)
	}
	if backend.filter != "42" || len(pending) != 1 || pending[0].MessageID != 7 {
		t.Fatalf("filter=%q pending=%+v", backend.filter, pending)
	}
	if err := inbox.MarkProcessed(context.Background(), message); err != nil {
		t.Fatal(err)
	}
	pending, err = inbox.Pending(context.Background(), 10)
	if err != nil || len(pending) != 0 {
		t.Fatalf("pending=%+v err=%v", pending, err)
	}
}

func TestStoreInboxRejectsDivergentPayloadIdentity(t *testing.T) {
	backend := &fakeInboxStore{}
	inbox, err := NewStoreInbox(backend, 42)
	if err != nil {
		t.Fatal(err)
	}
	if err := inbox.Save(context.Background(), Message{ScaleSetID: 43, SessionID: "x", MessageID: 1}); err == nil {
		t.Fatal("foreign scale set saved")
	}
	if err := inbox.Save(context.Background(), Message{ScaleSetID: 42, SessionID: "session", MessageID: 7}); err != nil {
		t.Fatal(err)
	}
	backend.row.MessageID = "8"
	if _, err := inbox.Pending(context.Background(), 10); err == nil {
		t.Fatal("mismatched stored identity accepted")
	}
	if err := inbox.MarkProcessed(context.Background(), Message{ScaleSetID: 43, SessionID: "session", MessageID: 7}); err == nil {
		t.Fatal("foreign message marked")
	}
}
