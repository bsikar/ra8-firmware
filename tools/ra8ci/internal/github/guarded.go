package github

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/actions/scaleset"
	"github.com/actions/scaleset/listener"
)

// Statistics is the nonsecret capacity snapshot included with a scale-set message.
type Statistics struct {
	Assigned   int
	Running    int
	Registered int
	Busy       int
	Idle       int
}

// Job identifies a GitHub assignment without carrying an acquire URL or token.
type Job struct {
	Kind            scaleset.MessageType
	RunnerRequestID int64
	Repository      string
	Owner           string
	JobID           string
	WorkflowRef     string
	DisplayName     string
	WorkflowRunID   int64
	EventName       string
	Labels          []string
	RunnerID        int
	RunnerName      string
	Result          string
	QueueTime       time.Time
	AssignTime      time.Time
	StartTime       time.Time
	FinishTime      time.Time
}

// Message is the durable inbox representation of one scale-set response.
// It deliberately excludes AcquireJobURL and session bearer credentials.
type Message struct {
	ScaleSetID int
	SessionID  string
	MessageID  int
	Statistics Statistics
	Available  []Job
	Assigned   []Job
	Started    []Job
	Completed  []Job
}

// Inbox must commit a message durably and idempotently before returning nil.
type Inbox interface {
	Save(context.Context, Message) error
}

// GuardedClient makes the upstream listener's acknowledge-before-handler order
// safe against loss of an unrecorded message. The inbox must replay saved
// messages independently on restart; this wrapper does not execute callbacks.
type GuardedClient struct {
	inner      listener.Client
	inbox      Inbox
	scaleSetID int
	mu         sync.Mutex
	recorded   map[int]string
}

var _ listener.Client = (*GuardedClient)(nil)

// NewGuardedClient rejects missing dependencies and invalid scale-set identity.
func NewGuardedClient(inner listener.Client, inbox Inbox, scaleSetID int) (*GuardedClient, error) {
	if inner == nil || inbox == nil || scaleSetID <= 0 {
		return nil, errors.New("github message client requires client, inbox, and positive scale-set ID")
	}
	return &GuardedClient{
		inner: inner, inbox: inbox, scaleSetID: scaleSetID, recorded: make(map[int]string),
	}, nil
}

// GetMessage commits a normalized response before exposing it to the listener.
func (c *GuardedClient) GetMessage(ctx context.Context, lastMessageID, maxCapacity int) (*scaleset.RunnerScaleSetMessage, error) {
	msg, err := c.inner.GetMessage(ctx, lastMessageID, maxCapacity)
	if err != nil || msg == nil {
		return msg, err
	}
	identity, err := sessionIdentity(c.inner.Session())
	if err != nil {
		return nil, err
	}
	entry, err := normalize(c.scaleSetID, identity, msg)
	if err != nil {
		return nil, err
	}
	if err := c.inbox.Save(ctx, entry); err != nil {
		return nil, fmt.Errorf("persist github message before acknowledgement: %w", err)
	}
	c.mu.Lock()
	c.recorded[msg.MessageID] = entry.SessionID
	c.mu.Unlock()
	return msg, nil
}

// DeleteMessage refuses to acknowledge anything not saved by this process,
// under the session now in force. A message identifier is the queue's
// sequence number and the queue belongs to the session, so a record written
// down under a session the client has since replaced says nothing about the
// message carrying that number today.
func (c *GuardedClient) DeleteMessage(ctx context.Context, messageID int) error {
	c.mu.Lock()
	recorded := c.recorded[messageID]
	c.mu.Unlock()
	identity, err := sessionIdentity(c.inner.Session())
	if err != nil {
		return err
	}
	if err := acknowledgedUnderThisSession(messageID, recorded, identity); err != nil {
		return err
	}
	if err := c.inner.DeleteMessage(ctx, messageID); err != nil {
		return err
	}
	c.mu.Lock()
	delete(c.recorded, messageID)
	c.mu.Unlock()
	return nil
}

// AcquireJobs delegates only after the listener has acknowledged a saved message.
func (c *GuardedClient) AcquireJobs(ctx context.Context, requestIDs []int64) ([]int64, error) {
	return c.inner.AcquireJobs(ctx, requestIDs)
}

// Session exposes upstream session metadata to the official listener.
func (c *GuardedClient) Session() scaleset.RunnerScaleSetSession {
	return c.inner.Session()
}

func normalize(scaleSetID int, sessionID string, src *scaleset.RunnerScaleSetMessage) (Message, error) {
	if src.MessageID <= 0 || src.Statistics == nil {
		return Message{}, errors.New("github message lacks ID or statistics")
	}
	out := Message{
		ScaleSetID: scaleSetID,
		SessionID:  sessionID,
		MessageID:  src.MessageID,
		Statistics: Statistics{
			Assigned:   src.Statistics.TotalAssignedJobs,
			Running:    src.Statistics.TotalRunningJobs,
			Registered: src.Statistics.TotalRegisteredRunners,
			Busy:       src.Statistics.TotalBusyRunners,
			Idle:       src.Statistics.TotalIdleRunners,
		},
	}
	for _, job := range src.JobAvailableMessages {
		if job == nil {
			return Message{}, errors.New("nil available job")
		}
		out.Available = append(out.Available, normalizeJob(job.JobMessageBase))
	}
	for _, job := range src.JobAssignedMessages {
		if job == nil {
			return Message{}, errors.New("nil assigned job")
		}
		out.Assigned = append(out.Assigned, normalizeJob(job.JobMessageBase))
	}
	for _, job := range src.JobStartedMessages {
		if job == nil {
			return Message{}, errors.New("nil started job")
		}
		entry := normalizeJob(job.JobMessageBase)
		entry.RunnerID, entry.RunnerName = job.RunnerID, job.RunnerName
		out.Started = append(out.Started, entry)
	}
	for _, job := range src.JobCompletedMessages {
		if job == nil {
			return Message{}, errors.New("nil completed job")
		}
		entry := normalizeJob(job.JobMessageBase)
		entry.RunnerID, entry.RunnerName, entry.Result = job.RunnerID, job.RunnerName, job.Result
		out.Completed = append(out.Completed, entry)
	}
	return out, nil
}

func normalizeJob(src scaleset.JobMessageBase) Job {
	return Job{
		Kind:            src.MessageType,
		RunnerRequestID: src.RunnerRequestID,
		Repository:      src.RepositoryName,
		Owner:           src.OwnerName,
		JobID:           src.JobID,
		WorkflowRef:     src.JobWorkflowRef,
		DisplayName:     src.JobDisplayName,
		WorkflowRunID:   src.WorkflowRunID,
		EventName:       src.EventName,
		Labels:          append([]string(nil), src.RequestLabels...),
		QueueTime:       src.QueueTime,
		AssignTime:      src.ScaleSetAssignTime,
		StartTime:       src.RunnerAssignTime,
		FinishTime:      src.FinishTime,
	}
}
