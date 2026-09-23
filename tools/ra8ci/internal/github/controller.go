package github

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/actions/scaleset"
	"github.com/actions/scaleset/listener"
)

// ReplayInbox exposes the durable unprocessed message queue. Pending must
// return messages in receipt order and MarkProcessed must be idempotent.
type ReplayInbox interface {
	Inbox
	Pending(context.Context, int) ([]Message, error)
	MarkProcessed(context.Context, Message) error
}

// Handler must make its effects idempotent by scale-set/session/message ID.
// Process records desired capacity and lifecycle events before returning; a
// controller crash may cause the same message to be delivered again.
type Handler interface {
	Process(context.Context, Message) error
	Reconcile(context.Context, Statistics) error
}

// Admission controls whether a GitHub job may be acquired or dispatched.
// Implementations should validate repository, workflow ref, event and labels
// against trusted configuration, not against untrusted job-provided commands.
type Admission interface {
	Allow(context.Context, Job) error
}

// Controller polls the official scale-set client while preserving an inbox
// across acknowledgement and callback failures. It never runs workflow code.
type Controller struct {
	client         listener.Client
	inbox          ReplayInbox
	handler        Handler
	admission      Admission
	scaleSetID     int
	maxRunners     int
	processTimeout time.Duration
}

// NewController requires every safety dependency and a bounded callback time.
func NewController(client listener.Client, inbox ReplayInbox, handler Handler, admission Admission, scaleSetID, maxRunners int, processTimeout time.Duration) (*Controller, error) {
	if client == nil || inbox == nil || handler == nil || admission == nil || scaleSetID <= 0 || maxRunners < 0 || processTimeout <= 0 {
		return nil, errors.New("github controller requires client, inbox, handler, admission, valid capacity and timeout")
	}
	guard, err := NewGuardedClient(client, inbox, scaleSetID)
	if err != nil {
		return nil, err
	}
	return &Controller{client: guard, inbox: inbox, handler: handler, admission: admission, scaleSetID: scaleSetID, maxRunners: maxRunners, processTimeout: processTimeout}, nil
}

// Run replays acknowledged messages before fetching new ones. A failure stops
// this listener so the supervisor can restart it without silently losing work.
func (c *Controller) Run(ctx context.Context) error {
	session := c.client.Session()
	if session.Statistics == nil {
		return errors.New("github session lacks statistics")
	}
	if err := c.replay(ctx); err != nil {
		return err
	}
	if err := c.reconcile(ctx, stats(session.Statistics)); err != nil {
		return err
	}
	lastReconcile := time.Now()
	lastMessageID := 0
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		message, err := c.client.GetMessage(ctx, lastMessageID, c.maxRunners)
		if err != nil {
			return fmt.Errorf("poll github scale set: %w", err)
		}
		if message == nil {
			timer := time.NewTimer(500 * time.Millisecond)
			select {
			case <-ctx.Done():
				if !timer.Stop() {
					<-timer.C
				}
				return ctx.Err()
			case <-timer.C:
			}
			if time.Since(lastReconcile) >= 15*time.Second {
				current := session.Statistics
				if current == nil {
					return errors.New("github session lost statistics")
				}
				if err := c.reconcile(ctx, stats(current)); err != nil {
					return err
				}
				lastReconcile = time.Now()
			}
			continue
		}
		entry, err := normalize(c.scaleSetID, c.client.Session().SessionID.String(), message)
		if err != nil {
			return err
		}
		if err := c.admit(ctx, entry); err != nil {
			return err
		}
		if err := c.client.DeleteMessage(ctx, message.MessageID); err != nil {
			return fmt.Errorf("acknowledge persisted github message: %w", err)
		}
		if err := c.process(ctx, entry); err != nil {
			return err
		}
		lastMessageID = message.MessageID
		session.Statistics = message.Statistics
	}
}

func (c *Controller) replay(ctx context.Context) error {
	for {
		pending, err := c.inbox.Pending(ctx, 64)
		if err != nil {
			return fmt.Errorf("read github inbox: %w", err)
		}
		if len(pending) == 0 {
			return nil
		}
		for _, entry := range pending {
			if entry.ScaleSetID != c.scaleSetID {
				return fmt.Errorf("foreign scale set %d in inbox", entry.ScaleSetID)
			}
			if err := c.admit(ctx, entry); err != nil {
				return err
			}
			if err := c.process(ctx, entry); err != nil {
				return err
			}
		}
	}
}

func (c *Controller) admit(ctx context.Context, entry Message) error {
	for _, list := range [][]Job{entry.Available, entry.Assigned, entry.Started, entry.Completed} {
		for _, job := range list {
			if err := c.admission.Allow(ctx, job); err != nil {
				return fmt.Errorf("github job admission: %w", err)
			}
		}
	}
	return nil
}

func (c *Controller) process(parent context.Context, entry Message) error {
	ctx, cancel := context.WithTimeout(context.WithoutCancel(parent), c.processTimeout)
	defer cancel()
	if len(entry.Available) > 0 {
		ids := make([]int64, 0, len(entry.Available))
		for _, job := range entry.Available {
			ids = append(ids, job.RunnerRequestID)
		}
		if _, err := c.client.AcquireJobs(ctx, ids); err != nil {
			return fmt.Errorf("acquire github jobs: %w", err)
		}
	}
	if err := c.handler.Process(ctx, entry); err != nil {
		return fmt.Errorf("process github message: %w", err)
	}
	if err := c.inbox.MarkProcessed(ctx, entry); err != nil {
		return fmt.Errorf("mark github message processed: %w", err)
	}
	return nil
}

func (c *Controller) reconcile(parent context.Context, statistics Statistics) error {
	ctx, cancel := context.WithTimeout(parent, c.processTimeout)
	defer cancel()
	if err := c.handler.Reconcile(ctx, statistics); err != nil {
		return fmt.Errorf("reconcile github scale set: %w", err)
	}
	return nil
}

func stats(src *scaleset.RunnerScaleSetStatistic) Statistics {
	return Statistics{Assigned: src.TotalAssignedJobs, Running: src.TotalRunningJobs, Registered: src.TotalRegisteredRunners, Busy: src.TotalBusyRunners, Idle: src.TotalIdleRunners}
}
