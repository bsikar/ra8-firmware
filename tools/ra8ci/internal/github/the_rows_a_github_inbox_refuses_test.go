// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// scriptedInboxStore answers with whatever a test planted, including a failure,
// so the adapter's own refusals can be told apart from the backend's.
type scriptedInboxStore struct {
	rows      []store.GitHubMessage
	listErr   error
	saveErr   error
	markErr   error
	asked     bool
	lastSet   string
	lastLimit int
	savedSet  string
	savedSess string
	savedID   string
	markedSet string
	markedID  string
}

func (s *scriptedInboxStore) SaveGitHubMessage(_ context.Context, set, session, id string, _ json.RawMessage) error {
	s.savedSet, s.savedSess, s.savedID = set, session, id
	return s.saveErr
}

func (s *scriptedInboxStore) MarkGitHubMessageProcessed(_ context.Context, set, _, id string) error {
	s.markedSet, s.markedID = set, id
	return s.markErr
}

func (s *scriptedInboxStore) ListPendingGitHubMessagesForScaleSet(_ context.Context, set string, limit int) ([]store.GitHubMessage, error) {
	s.asked = true
	s.lastSet, s.lastLimit = set, limit
	if s.listErr != nil {
		return nil, s.listErr
	}
	return s.rows, nil
}

// storedRow encodes a message the way Save would have, then lets the caller
// disagree with it, which is how a corrupted or mixed-up row is modelled.
func storedRow(t *testing.T, set, session, id string, message Message) store.GitHubMessage {
	t.Helper()
	payload, err := json.Marshal(message)
	if err != nil {
		t.Fatalf("marshal fixture message: %v", err)
	}
	return store.GitHubMessage{ScaleSetID: set, SessionID: session, MessageID: id, Payload: payload}
}

func TestAnInboxWithoutAStoreOrItsOwnScaleSetIsNotBuilt(t *testing.T) {
	backend := &scriptedInboxStore{}
	for _, c := range []struct {
		name    string
		backend InboxStore
		scale   int
	}{
		{"no store at all", nil, 42},
		{"a scale set of zero", backend, 0},
		{"a negative scale set", backend, -1},
	} {
		t.Run(c.name, func(t *testing.T) {
			inbox, err := NewStoreInbox(c.backend, c.scale)
			if err == nil {
				t.Fatal("inbox built without what it needs")
			}
			if inbox != nil {
				t.Fatalf("refused inbox still handed back: %+v", inbox)
			}
			if !strings.Contains(err.Error(), "positive scale-set ID") {
				t.Fatalf("refusal does not say what is missing: %v", err)
			}
		})
	}
	if _, err := NewStoreInbox(backend, 1); err != nil {
		t.Fatalf("lowest usable scale set refused: %v", err)
	}
}

func TestABackendThatCannotListIsReportedRatherThanReadAsAnEmptyPage(t *testing.T) {
	failure := errors.New("connection reset by the database")
	backend := &scriptedInboxStore{listErr: failure}
	inbox, err := NewStoreInbox(backend, 42)
	if err != nil {
		t.Fatal(err)
	}
	pending, err := inbox.Pending(context.Background(), 10)
	if !errors.Is(err, failure) {
		t.Fatalf("backend failure not handed back: %v", err)
	}
	if pending != nil {
		t.Fatalf("page returned beside a failure: %+v", pending)
	}
}

func TestAStoredRowThatIsNotAMessageIsRefusedByName(t *testing.T) {
	for _, c := range []struct {
		name    string
		payload string
	}{
		{"a truncated object", "{"},
		{"a bare string", `"session"`},
		{"nothing at all", ""},
	} {
		t.Run(c.name, func(t *testing.T) {
			backend := &scriptedInboxStore{rows: []store.GitHubMessage{{
				ScaleSetID: "42",
				SessionID:  "session",
				MessageID:  "7",
				Payload:    json.RawMessage(c.payload),
			}}}
			inbox, err := NewStoreInbox(backend, 42)
			if err != nil {
				t.Fatal(err)
			}
			pending, err := inbox.Pending(context.Background(), 10)
			if err == nil {
				t.Fatal("undecodable row accepted")
			}
			if !strings.Contains(err.Error(), "decode github message") {
				t.Fatalf("refusal does not name the decode: %v", err)
			}
			if pending != nil {
				t.Fatalf("page returned beside a failure: %+v", pending)
			}
		})
	}
}

func TestAPageOutsideTheInboxBoundsIsRefusedBeforeTheStoreIsAsked(t *testing.T) {
	for _, limit := range []int{0, -1, 1001} {
		backend := &scriptedInboxStore{}
		inbox, err := NewStoreInbox(backend, 42)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := inbox.Pending(context.Background(), limit); err == nil {
			t.Fatalf("page of %d accepted", limit)
		}
		if backend.asked {
			t.Fatalf("store asked for a page of %d", limit)
		}
	}
	for _, limit := range []int{1, 1000} {
		backend := &scriptedInboxStore{}
		inbox, err := NewStoreInbox(backend, 42)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := inbox.Pending(context.Background(), limit); err != nil {
			t.Fatalf("page of %d refused: %v", limit, err)
		}
		if !backend.asked || backend.lastLimit != limit || backend.lastSet != "42" {
			t.Fatalf("asked=%v limit=%d set=%q", backend.asked, backend.lastLimit, backend.lastSet)
		}
	}
}

func TestARowThatDoesNotAnswerForItsOwnMessageIsRefused(t *testing.T) {
	held := Message{ScaleSetID: 42, SessionID: "session", MessageID: 7}
	for _, c := range []struct {
		name string
		row  func(*testing.T) store.GitHubMessage
	}{
		{"the row names another scale set", func(t *testing.T) store.GitHubMessage {
			return storedRow(t, "43", "session", "7", held)
		}},
		{"the row names another session", func(t *testing.T) store.GitHubMessage {
			return storedRow(t, "42", "other", "7", held)
		}},
		{"the payload names another scale set", func(t *testing.T) store.GitHubMessage {
			return storedRow(t, "42", "session", "7", Message{ScaleSetID: 7, SessionID: "session", MessageID: 7})
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			backend := &scriptedInboxStore{rows: []store.GitHubMessage{c.row(t)}}
			inbox, err := NewStoreInbox(backend, 42)
			if err != nil {
				t.Fatal(err)
			}
			pending, err := inbox.Pending(context.Background(), 10)
			if err == nil {
				t.Fatal("divergent row accepted")
			}
			if !strings.Contains(err.Error(), "payload identity mismatch") {
				t.Fatalf("refusal does not name the mismatch: %v", err)
			}
			if pending != nil {
				t.Fatalf("page returned beside a failure: %+v", pending)
			}
		})
	}
	backend := &scriptedInboxStore{rows: []store.GitHubMessage{storedRow(t, "42", "session", "7", held)}}
	inbox, err := NewStoreInbox(backend, 42)
	if err != nil {
		t.Fatal(err)
	}
	pending, err := inbox.Pending(context.Background(), 10)
	if err != nil || len(pending) != 1 || pending[0].MessageID != 7 {
		t.Fatalf("an agreeing row was not read back: pending=%+v err=%v", pending, err)
	}
}

func TestSaveAndMarkCarryTheInboxScaleSetNotTheMessages(t *testing.T) {
	backend := &scriptedInboxStore{}
	inbox, err := NewStoreInbox(backend, 42)
	if err != nil {
		t.Fatal(err)
	}
	message := Message{ScaleSetID: 42, SessionID: "session", MessageID: 7}
	if err := inbox.Save(context.Background(), message); err != nil {
		t.Fatal(err)
	}
	if backend.savedSet != "42" || backend.savedSess != "session" || backend.savedID != "7" {
		t.Fatalf("saved set=%q session=%q id=%q", backend.savedSet, backend.savedSess, backend.savedID)
	}
	if err := inbox.MarkProcessed(context.Background(), message); err != nil {
		t.Fatal(err)
	}
	if backend.markedSet != "42" || backend.markedID != "7" {
		t.Fatalf("marked set=%q id=%q", backend.markedSet, backend.markedID)
	}

	saveFailure := errors.New("write refused")
	markFailure := errors.New("mark refused")
	failing := &scriptedInboxStore{saveErr: saveFailure, markErr: markFailure}
	inbox, err = NewStoreInbox(failing, 42)
	if err != nil {
		t.Fatal(err)
	}
	if err := inbox.Save(context.Background(), message); !errors.Is(err, saveFailure) {
		t.Fatalf("save failure not handed back: %v", err)
	}
	if err := inbox.MarkProcessed(context.Background(), message); !errors.Is(err, markFailure) {
		t.Fatalf("mark failure not handed back: %v", err)
	}
}

func TestAnInboxThatWasNeverBuiltRefusesRatherThanCrashes(t *testing.T) {
	var inbox *StoreInbox
	message := Message{ScaleSetID: 42, SessionID: "session", MessageID: 7}
	if err := inbox.Save(context.Background(), message); err == nil {
		t.Fatal("nil inbox saved")
	}
	if err := inbox.MarkProcessed(context.Background(), message); err == nil {
		t.Fatal("nil inbox marked")
	}
	pending, err := inbox.Pending(context.Background(), 10)
	if err == nil {
		t.Fatal("nil inbox paged")
	}
	if pending != nil {
		t.Fatalf("page returned by a nil inbox: %+v", pending)
	}
}

func TestAMessageThatDoesNotBelongToThisInboxIsNeverWritten(t *testing.T) {
	for _, c := range []struct {
		name    string
		message Message
	}{
		{"another scale set", Message{ScaleSetID: 43, SessionID: "session", MessageID: 7}},
		{"no session", Message{ScaleSetID: 42, MessageID: 7}},
		{"no message ID", Message{ScaleSetID: 42, SessionID: "session"}},
		{"a negative message ID", Message{ScaleSetID: 42, SessionID: "session", MessageID: -1}},
	} {
		t.Run(c.name, func(t *testing.T) {
			backend := &scriptedInboxStore{}
			inbox, err := NewStoreInbox(backend, 42)
			if err != nil {
				t.Fatal(err)
			}
			if err := inbox.Save(context.Background(), c.message); err == nil {
				t.Fatal("foreign message saved")
			}
			if err := inbox.MarkProcessed(context.Background(), c.message); err == nil {
				t.Fatal("foreign message marked")
			}
			if backend.savedID != "" || backend.markedID != "" {
				t.Fatalf("store reached: saved=%q marked=%q", backend.savedID, backend.markedID)
			}
		})
	}
}
