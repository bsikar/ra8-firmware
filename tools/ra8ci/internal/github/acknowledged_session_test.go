// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/google/uuid"
)

// persisted drives one message through the guard and returns the guard and
// the client behind it, with message 7 written down under the client's
// current session.
func persisted(t *testing.T) (*GuardedClient, *fakeClient, *fakeInbox) {
	t.Helper()
	client := testClient()
	inbox := &fakeInbox{}
	guard, err := NewGuardedClient(client, inbox, 42)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := guard.GetMessage(context.Background(), 0, 1); err != nil {
		t.Fatal(err)
	}
	if inbox.saves != 1 {
		t.Fatalf("saves = %d", inbox.saves)
	}
	return guard, client, inbox
}

func TestAMessageRecordedUnderAnotherSessionIsNotAcknowledged(t *testing.T) {
	guard, client, _ := persisted(t)
	client.session.SessionID = uuid.New()
	err := guard.DeleteMessage(context.Background(), 7)
	if err == nil {
		t.Fatal("a record from the previous session acknowledged a message under the new one")
	}
	if !errors.Is(err, ErrForeignSessionAcknowledgement) {
		t.Fatalf("err = %v, want ErrForeignSessionAcknowledgement", err)
	}
	if len(client.deleted) != 0 {
		t.Fatalf("the acknowledgement reached the inner client: %v", client.deleted)
	}
}

func TestARefusedAcknowledgementLeavesTheRecordStanding(t *testing.T) {
	guard, client, _ := persisted(t)
	original := client.session.SessionID
	client.session.SessionID = uuid.New()
	if err := guard.DeleteMessage(context.Background(), 7); err == nil {
		t.Fatal("foreign session accepted")
	}
	// The message is still saved and still ours. A refusal must not
	// consume the record, or the session moving back would leave a
	// message nothing can ever acknowledge.
	client.session.SessionID = original
	if err := guard.DeleteMessage(context.Background(), 7); err != nil {
		t.Fatalf("the record did not survive the refusal: %v", err)
	}
	if len(client.deleted) != 1 || client.deleted[0] != 7 {
		t.Fatalf("deleted %v", client.deleted)
	}
}

func TestARefreshThatKeepsItsSessionAcknowledgesNormally(t *testing.T) {
	// The ordinary refresh: the token is reissued, the session identifier
	// is the same, the queue and its numbering carry on. The rule is
	// one-sided and must not fire here.
	guard, client, _ := persisted(t)
	client.session.MessageQueueAccessToken = "reissued-session-token"
	if err := guard.DeleteMessage(context.Background(), 7); err != nil {
		t.Fatalf("an ordinary token refresh was read as a foreign session: %v", err)
	}
	if len(client.deleted) != 1 {
		t.Fatalf("deleted %v", client.deleted)
	}
}

func TestAnUnrecordedMessageIsRefusedWhateverTheSessionSays(t *testing.T) {
	client := testClient()
	guard, err := NewGuardedClient(client, &fakeInbox{}, 42)
	if err != nil {
		t.Fatal(err)
	}
	err = guard.DeleteMessage(context.Background(), 7)
	if err == nil {
		t.Fatal("a message nothing persisted was acknowledged")
	}
	if errors.Is(err, ErrForeignSessionAcknowledgement) {
		t.Fatalf("a missing record was reported as a session disagreement: %v", err)
	}
	if !strings.Contains(err.Error(), "not persisted") {
		t.Fatalf("err = %v", err)
	}
}

func TestTheSameIDUnderANewSessionIsAcknowledgedOnlyOnceItIsSavedAgain(t *testing.T) {
	// The whole hazard in one test: a new session hands out its own
	// message 7. It is acknowledged only after this process has written
	// that message down, never on the strength of the old record.
	guard, client, inbox := persisted(t)
	client.session.SessionID = uuid.New()
	if err := guard.DeleteMessage(context.Background(), 7); err == nil {
		t.Fatal("the new session's message 7 was acknowledged on the old record")
	}
	if _, err := guard.GetMessage(context.Background(), 0, 1); err != nil {
		t.Fatal(err)
	}
	if inbox.saves != 2 {
		t.Fatalf("saves = %d, the new session's message was not written down", inbox.saves)
	}
	if err := guard.DeleteMessage(context.Background(), 7); err != nil {
		t.Fatalf("the re-saved message was not acknowledged: %v", err)
	}
	if len(client.deleted) != 1 || client.deleted[0] != 7 {
		t.Fatalf("deleted %v", client.deleted)
	}
}

func TestBothDoorsRefuseASessionWithNoIdentity(t *testing.T) {
	// The persist door already refused a nil session. The acknowledge
	// door reaches the same rule, so neither can drift into treating the
	// zero UUID as an identity two records can agree on.
	guard, client, _ := persisted(t)
	client.session.SessionID = uuid.Nil
	if _, err := guard.GetMessage(context.Background(), 0, 1); err == nil {
		t.Fatal("a nil session persisted a message")
	}
	if err := guard.DeleteMessage(context.Background(), 7); err == nil {
		t.Fatal("a nil session acknowledged a message")
	}
	if len(client.deleted) != 0 {
		t.Fatalf("the acknowledgement reached the inner client: %v", client.deleted)
	}
}

func TestSessionIdentityRefusesOnlyTheAbsentOnes(t *testing.T) {
	client := testClient()
	identity, err := sessionIdentity(client.session)
	if err != nil {
		t.Fatal(err)
	}
	if identity != client.session.SessionID.String() {
		t.Fatalf("identity = %q", identity)
	}
	client.session.SessionID = uuid.Nil
	if _, err := sessionIdentity(client.session); err == nil {
		t.Fatal("the zero UUID answered as an identity")
	}
	if client.session.SessionID.String() != nilSessionID {
		t.Fatalf("nilSessionID does not render the zero UUID: %q", client.session.SessionID.String())
	}
}

func TestAcknowledgedUnderThisSession(t *testing.T) {
	const a = "0f4cbe8e-2a1f-4d2a-9a3f-3a5f1f0a1b2c"
	const b = "1a2b3c4d-5e6f-4a8b-9c0d-1e2f3a4b5c6d"
	for _, row := range []struct {
		name     string
		recorded string
		current  string
		accepted bool
		foreign  bool
	}{
		{name: "the session that saved it", recorded: a, current: a, accepted: true},
		{name: "a different session", recorded: a, current: b, foreign: true},
		{name: "no record at all", recorded: "", current: a},
		{name: "no record and no session", recorded: "", current: ""},
		{name: "a record against a blank session", recorded: a, current: "", foreign: true},
	} {
		t.Run(row.name, func(t *testing.T) {
			err := acknowledgedUnderThisSession(7, row.recorded, row.current)
			if row.accepted {
				if err != nil {
					t.Fatalf("err = %v, want nil", err)
				}
				return
			}
			if err == nil {
				t.Fatal("accepted")
			}
			if errors.Is(err, ErrForeignSessionAcknowledgement) != row.foreign {
				t.Fatalf("err = %v, foreign want %v", err, row.foreign)
			}
		})
	}
}

func TestTheRefusalNamesBothSessions(t *testing.T) {
	guard, client, _ := persisted(t)
	original := client.session.SessionID.String()
	client.session.SessionID = uuid.New()
	current := client.session.SessionID.String()
	err := guard.DeleteMessage(context.Background(), 7)
	if err == nil {
		t.Fatal("foreign session accepted")
	}
	if !strings.Contains(err.Error(), original) || !strings.Contains(err.Error(), current) {
		t.Fatalf("err = %v, want both %s and %s", err, original, current)
	}
}
