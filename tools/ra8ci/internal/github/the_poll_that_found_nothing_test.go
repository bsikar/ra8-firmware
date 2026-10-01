// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/actions/scaleset"
	"github.com/google/uuid"
)

// A queue with nothing in it is the listener's ordinary state, not an idle
// process. It has to keep asking, and it has to stop the moment the
// supervisor cancels it rather than sitting out the poll interval first.

// idleClient answers a scripted sequence of polls, nil entries included,
// which the queue-backed fixture cannot express: it answers nil only once it
// has run out.
type idleClient struct {
	session scaleset.RunnerScaleSetSession
	script  []*scaleset.RunnerScaleSetMessage
	gets    int
	onGet   func(int)
}

func (c *idleClient) GetMessage(context.Context, int, int) (*scaleset.RunnerScaleSetMessage, error) {
	c.gets++
	if c.onGet != nil {
		c.onGet(c.gets)
	}
	if c.gets > len(c.script) {
		return nil, nil
	}
	return c.script[c.gets-1], nil
}

func (c *idleClient) DeleteMessage(context.Context, int) error { return nil }

func (c *idleClient) AcquireJobs(_ context.Context, ids []int64) ([]int64, error) { return ids, nil }

func (c *idleClient) Session() scaleset.RunnerScaleSetSession { return c.session }

// polling builds a controller over an idleClient and an empty inbox.
func polling(t *testing.T, client *idleClient) (*Controller, *countingHandler) {
	t.Helper()
	client.session = scaleset.RunnerScaleSetSession{
		SessionID:  uuid.New(),
		Statistics: &scaleset.RunnerScaleSetStatistic{TotalIdleRunners: 1},
	}
	handler := &countingHandler{}
	controller, err := NewController(client, &scriptedInbox{}, handler, testAdmission{}, 42, 2, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	return controller, handler
}

// An empty poll is waited out and asked again, and the message that arrives
// next is carried through as any other would be.
func TestAnEmptyPollIsAskedAgainRatherThanEndingTheListener(t *testing.T) {
	client := &idleClient{script: []*scaleset.RunnerScaleSetMessage{nil, oneAvailable(7)}}
	controller, handler := polling(t, client)
	ctx, cancel := context.WithCancel(context.Background())
	client.onGet = func(n int) {
		// Stop the listener once the message after the empty poll has
		// been asked for, so the run ends on the supervisor's terms.
		if n >= 3 {
			cancel()
		}
	}
	defer cancel()

	started := time.Now()
	err := controller.Run(ctx)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("Run = %v", err)
	}
	if client.gets < 3 {
		t.Fatalf("the listener asked %d times, so the empty poll ended it", client.gets)
	}
	if handler.processed != 1 {
		t.Fatalf("the message after the empty poll was processed %d times", handler.processed)
	}
	if waited := time.Since(started); waited < 400*time.Millisecond {
		t.Fatalf("the empty poll was retried after %s, so nothing was waited out", waited)
	}
}

// A cancellation during that wait ends the listener then and there. Sitting
// out the rest of the interval would hold a supervisor's shutdown open for
// no reason, and the error reported is the context's own.
func TestACancellationDuringTheIdleWaitEndsTheListenerAtOnce(t *testing.T) {
	client := &idleClient{}
	controller, handler := polling(t, client)
	ctx, cancel := context.WithCancel(context.Background())
	client.onGet = func(int) {
		go func() {
			time.Sleep(20 * time.Millisecond)
			cancel()
		}()
	}
	defer cancel()

	started := time.Now()
	err := controller.Run(ctx)
	waited := time.Since(started)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("Run = %v", err)
	}
	if waited > 400*time.Millisecond {
		t.Fatalf("the listener sat out %s of the poll interval after being cancelled", waited)
	}
	if client.gets != 1 {
		t.Fatalf("the listener asked %d times after it was cancelled", client.gets)
	}
	if handler.processed != 0 {
		t.Fatalf("a cancelled listener processed %d messages", handler.processed)
	}
}

// A session with no statistics is refused before the inbox is read, and a
// controller missing any of its parts is refused before it exists. Neither
// can be discovered on the first poll of a shift.
func TestAControllerWithoutItsPartsIsNeverBuilt(t *testing.T) {
	client := &idleClient{session: scaleset.RunnerScaleSetSession{SessionID: uuid.New()}}
	inbox := &scriptedInbox{}
	handler := &countingHandler{}

	for _, missing := range []struct {
		name       string
		client     *idleClient
		inbox      ReplayInbox
		handler    Handler
		admission  Admission
		scaleSetID int
		maxRunners int
		timeout    time.Duration
	}{
		{name: "no client", inbox: inbox, handler: handler, admission: testAdmission{}, scaleSetID: 42, maxRunners: 2, timeout: time.Second},
		{name: "no inbox", client: client, handler: handler, admission: testAdmission{}, scaleSetID: 42, maxRunners: 2, timeout: time.Second},
		{name: "no handler", client: client, inbox: inbox, admission: testAdmission{}, scaleSetID: 42, maxRunners: 2, timeout: time.Second},
		{name: "no admission", client: client, inbox: inbox, handler: handler, scaleSetID: 42, maxRunners: 2, timeout: time.Second},
		{name: "no scale set", client: client, inbox: inbox, handler: handler, admission: testAdmission{}, maxRunners: 2, timeout: time.Second},
		{name: "negative capacity", client: client, inbox: inbox, handler: handler, admission: testAdmission{}, scaleSetID: 42, maxRunners: -1, timeout: time.Second},
		{name: "no timeout", client: client, inbox: inbox, handler: handler, admission: testAdmission{}, scaleSetID: 42, maxRunners: 2},
		{name: "negative timeout", client: client, inbox: inbox, handler: handler, admission: testAdmission{}, scaleSetID: 42, maxRunners: 2, timeout: -time.Second},
	} {
		var built *Controller
		var err error
		if missing.client == nil {
			built, err = NewController(nil, missing.inbox, missing.handler, missing.admission,
				missing.scaleSetID, missing.maxRunners, missing.timeout)
		} else {
			built, err = NewController(missing.client, missing.inbox, missing.handler, missing.admission,
				missing.scaleSetID, missing.maxRunners, missing.timeout)
		}
		if err == nil || built != nil {
			t.Fatalf("%s built a controller: %v", missing.name, err)
		}
	}

	// A capacity of zero is a listener that may hold no runners, which is
	// how a scale set is drained. It is not a missing part.
	drained, err := NewController(client, inbox, handler, testAdmission{}, 42, 0, time.Second)
	if err != nil || drained == nil {
		t.Fatalf("a drained listener was refused: %v", err)
	}
	if err := drained.Run(context.Background()); err == nil ||
		err.Error() != "github session lacks statistics" {
		t.Fatalf("a session with no statistics answered %v", err)
	}
}
