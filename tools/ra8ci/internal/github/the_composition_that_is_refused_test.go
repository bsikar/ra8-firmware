// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"strings"
	"testing"
	"time"
)

// Composition is the last place a half-built session can be caught. Past it
// the controller owns the session and the deferred cleanup paths dereference
// both, so every incomplete dependency is refused here rather than at the
// first message.
func TestComposingAControllerSessionRefusesAnIncompleteSession(t *testing.T) {
	whole := func() *Session {
		return &Session{Client: &fakeClient{}, close: func(context.Context) error { return nil }, scaleSetID: 7}
	}
	factory := func(*Session) (Handler, error) { return &testHandler{}, nil }

	withoutClient := whole()
	withoutClient.Client = nil
	withoutClose := whole()
	withoutClose.close = nil
	unnumbered := whole()
	unnumbered.scaleSetID = 0

	for _, c := range []struct {
		name      string
		session   *Session
		inbox     ReplayInbox
		admission Admission
		factory   HandlerFactory
	}{
		{"no session", nil, &fakeInbox{}, testAdmission{}, factory},
		{"a session with no client", withoutClient, &fakeInbox{}, testAdmission{}, factory},
		{"a session with no close", withoutClose, &fakeInbox{}, testAdmission{}, factory},
		{"a session with no scale set", unnumbered, &fakeInbox{}, testAdmission{}, factory},
		{"no inbox", whole(), nil, testAdmission{}, factory},
		{"no admission", whole(), &fakeInbox{}, nil, factory},
		{"no handler factory", whole(), &fakeInbox{}, testAdmission{}, nil},
	} {
		t.Run(c.name, func(t *testing.T) {
			bound, err := ComposeControllerSession(c.session, c.inbox, c.admission, 2, time.Second, c.factory)
			if err == nil {
				t.Fatal("composition accepted an incomplete set of dependencies")
			}
			if bound != nil {
				t.Fatal("a refused composition handed back a controller session")
			}
			if !strings.Contains(err.Error(), "controller composition requires an open session") {
				t.Fatalf("refusal does not name the composition: %v", err)
			}
		})
	}
}

// Run closes the session on the way out, so a half-built controller session is
// refused before the run rather than during the cleanup that follows it.
func TestRunningAHalfBuiltControllerSessionIsRefused(t *testing.T) {
	session := &Session{Client: &fakeClient{}, close: func(context.Context) error { return nil }, scaleSetID: 7}

	for _, c := range []struct {
		name  string
		bound *ControllerSession
		ctx   context.Context
	}{
		{"no controller session", nil, context.Background()},
		{"neither controller nor session", &ControllerSession{}, context.Background()},
		{"no controller", &ControllerSession{session: session}, context.Background()},
		{"no session", &ControllerSession{controller: &Controller{}}, context.Background()},
		{"no context", &ControllerSession{controller: &Controller{}, session: session}, nil},
	} {
		t.Run(c.name, func(t *testing.T) {
			err := c.bound.Run(c.ctx)
			if err == nil {
				t.Fatal("running a half-built controller session answered nil")
			}
			if !strings.Contains(err.Error(), "invalid GitHub controller session") {
				t.Fatalf("refusal does not name the controller session: %v", err)
			}
		})
	}
}
