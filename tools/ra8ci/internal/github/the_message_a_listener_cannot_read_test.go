// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"strings"
	"testing"

	"github.com/actions/scaleset"
)

// A message GitHub sends that this plane cannot make sense of is the one
// case where stopping the listener is not obviously right: the message is
// already delivered, and refusing it means it is still there on the next
// poll. It stops anyway, unacknowledged, because a message whose shape is
// not understood is exactly the one that must not be silently dropped.
// the_steps_a_listener_stops_at_test.go pins the remote steps around this
// one; this pins the message itself.

// A message without an ID or without a capacity snapshot cannot be recorded
// against anything, so the listener stops before the inbox is written and
// before the message is acknowledged.
func TestAMessageThatCannotBeNormalizedStopsTheListenerUnacknowledged(t *testing.T) {
	for name, message := range map[string]*scaleset.RunnerScaleSetMessage{
		"no identifier": {
			MessageID:  0,
			Statistics: &scaleset.RunnerScaleSetStatistic{TotalAssignedJobs: 1},
		},
		"a negative identifier": {
			MessageID:  -3,
			Statistics: &scaleset.RunnerScaleSetStatistic{TotalAssignedJobs: 1},
		},
		"no capacity snapshot": {MessageID: 11},
	} {
		t.Run(name, func(t *testing.T) {
			client := &scriptedClient{queue: []*scaleset.RunnerScaleSetMessage{message}}
			inbox := &scriptedInbox{}
			handler := &countingHandler{}

			err := listening(t, client, inbox, handler).Run(context.Background())
			if err == nil || !strings.Contains(err.Error(), "lacks ID or statistics") {
				t.Fatalf("an unreadable message: %v", err)
			}
			if len(client.deleted) != 0 {
				t.Fatalf("an unreadable message was acknowledged: %v", client.deleted)
			}
			if inbox.saves != 0 || handler.processed != 0 {
				t.Fatalf("saves=%d processed=%d", inbox.saves, handler.processed)
			}
		})
	}
}

// A message carrying a nil job in any of its four lists is refused the same
// way. The lists are read in order, so a good message with one bad entry is
// still refused whole rather than partly recorded.
func TestAMessageCarryingANilJobIsRefusedWhole(t *testing.T) {
	statistics := func() *scaleset.RunnerScaleSetStatistic {
		return &scaleset.RunnerScaleSetStatistic{TotalAssignedJobs: 1}
	}
	for name, message := range map[string]*scaleset.RunnerScaleSetMessage{
		"available": {MessageID: 21, Statistics: statistics(),
			JobAvailableMessages: []*scaleset.JobAvailable{nil}},
		"assigned": {MessageID: 22, Statistics: statistics(),
			JobAssignedMessages: []*scaleset.JobAssigned{nil}},
		"started": {MessageID: 23, Statistics: statistics(),
			JobStartedMessages: []*scaleset.JobStarted{nil}},
		"completed": {MessageID: 24, Statistics: statistics(),
			JobCompletedMessages: []*scaleset.JobCompleted{nil}},
	} {
		t.Run(name, func(t *testing.T) {
			client := &scriptedClient{queue: []*scaleset.RunnerScaleSetMessage{message}}
			inbox := &scriptedInbox{}
			handler := &countingHandler{}

			err := listening(t, client, inbox, handler).Run(context.Background())
			if err == nil || !strings.Contains(err.Error(), "nil "+name+" job") {
				t.Fatalf("a nil %s job: %v", name, err)
			}
			if len(client.deleted) != 0 || inbox.saves != 0 || handler.processed != 0 {
				t.Fatalf("deleted=%v saves=%d processed=%d", client.deleted, inbox.saves, handler.processed)
			}
		})
	}
}
