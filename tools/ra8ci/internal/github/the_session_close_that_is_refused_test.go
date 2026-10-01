// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// Closing the remote message session is the last thing this plane owes GitHub,
// and it runs from a deferred cleanup path where the session may be half built
// or absent. A nil session, a session carrying no close, and a call with no
// context are all refused rather than dereferenced, because a panic there
// would take down the process on the way out and leave the remote session
// behind.
func TestClosingASessionThatWasNeverOpenedIsRefused(t *testing.T) {
	for _, c := range []struct {
		name    string
		session *Session
		ctx     context.Context
	}{
		{"no session at all", nil, context.Background()},
		{"a session carrying no close", &Session{}, context.Background()},
		{"no context", &Session{close: func(context.Context) error { return nil }}, nil},
	} {
		t.Run(c.name, func(t *testing.T) {
			err := c.session.Close(c.ctx)
			if err == nil {
				t.Fatal("closing a session that was never opened answered nil")
			}
			if !strings.Contains(err.Error(), "invalid GitHub message session") {
				t.Fatalf("refusal does not name the session: %v", err)
			}
		})
	}
}

// A session that was opened hands the context straight to the remote close and
// reports what it answered. The deferred cleanup paths in this file join that
// error with the run's own, so swallowing it would hide a session GitHub still
// holds.
func TestClosingAnOpenSessionReportsWhatTheRemoteAnswered(t *testing.T) {
	refused := errors.New("remote refused the delete")
	calls := 0
	var handed context.Context
	session := &Session{close: func(ctx context.Context) error {
		calls++
		handed = ctx
		return refused
	}}

	ctx := context.WithValue(context.Background(), sessionCloseKeyForTest, "handed")
	if err := session.Close(ctx); !errors.Is(err, refused) {
		t.Fatalf("Close answered %v, want the remote refusal", err)
	}
	if calls != 1 {
		t.Fatalf("remote close called %d times, want exactly one", calls)
	}
	if handed != ctx {
		t.Fatal("Close did not hand the caller's context to the remote close")
	}

	calls = 0
	session = &Session{close: func(context.Context) error { calls++; return nil }}
	if err := session.Close(context.Background()); err != nil {
		t.Fatalf("closing an open session answered %v", err)
	}
	if calls != 1 {
		t.Fatalf("remote close called %d times, want exactly one", calls)
	}
}

type sessionCloseKey struct{}

var sessionCloseKeyForTest = sessionCloseKey{}
