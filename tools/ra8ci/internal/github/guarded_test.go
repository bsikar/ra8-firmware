package github

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/actions/scaleset"
	"github.com/google/uuid"
)

type fakeClient struct {
	message  *scaleset.RunnerScaleSetMessage
	session  scaleset.RunnerScaleSetSession
	deleted  []int
	acquired [][]int64
	once     bool
	gets     int
}

func (f *fakeClient) GetMessage(context.Context, int, int) (*scaleset.RunnerScaleSetMessage, error) {
	if f.once && f.gets > 0 {
		return nil, nil
	}
	f.gets++
	return f.message, nil
}

func (f *fakeClient) DeleteMessage(_ context.Context, id int) error {
	f.deleted = append(f.deleted, id)
	return nil
}

func (f *fakeClient) AcquireJobs(_ context.Context, ids []int64) ([]int64, error) {
	f.acquired = append(f.acquired, append([]int64(nil), ids...))
	return ids, nil
}

func (f *fakeClient) Session() scaleset.RunnerScaleSetSession { return f.session }

type fakeInbox struct {
	message Message
	err     error
	saves   int
	pending []Message
}

func (f *fakeInbox) Save(_ context.Context, msg Message) error {
	f.saves++
	f.message = msg
	if f.err == nil {
		f.pending = append(f.pending, msg)
	}
	return f.err
}

func (f *fakeInbox) Pending(context.Context, int) ([]Message, error) {
	return append([]Message(nil), f.pending...), nil
}

func (f *fakeInbox) MarkProcessed(_ context.Context, msg Message) error {
	for i, entry := range f.pending {
		if entry.ScaleSetID == msg.ScaleSetID && entry.SessionID == msg.SessionID && entry.MessageID == msg.MessageID {
			f.pending = append(f.pending[:i], f.pending[i+1:]...)
			return nil
		}
	}
	return errors.New("message not pending")
}

func TestGuardedClientPersistsBeforeAcknowledgement(t *testing.T) {
	client := testClient()
	inbox := &fakeInbox{err: errors.New("database unavailable")}
	guard, err := NewGuardedClient(client, inbox, 42)
	if err != nil {
		t.Fatal(err)
	}
	if err := guard.DeleteMessage(context.Background(), 7); err == nil {
		t.Fatal("unpersisted message acknowledged")
	}
	if _, err := guard.GetMessage(context.Background(), 0, 1); err == nil {
		t.Fatal("failed persistence was ignored")
	}
	if len(client.deleted) != 0 {
		t.Fatal("message acknowledged after failed persistence")
	}
	inbox.err = nil
	if _, err := guard.GetMessage(context.Background(), 0, 1); err != nil {
		t.Fatal(err)
	}
	if err := guard.DeleteMessage(context.Background(), 7); err != nil {
		t.Fatal(err)
	}
	if len(client.deleted) != 1 || client.deleted[0] != 7 {
		t.Fatalf("deleted %v", client.deleted)
	}
	if inbox.saves != 2 {
		t.Fatalf("saves = %d", inbox.saves)
	}
	if err := guard.DeleteMessage(context.Background(), 7); err == nil {
		t.Fatal("duplicate acknowledgement allowed")
	}
}

func TestGuardedClientDropsSecretsAndRetainsPolicyFields(t *testing.T) {
	client := testClient()
	inbox := &fakeInbox{}
	guard, err := NewGuardedClient(client, inbox, 42)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := guard.GetMessage(context.Background(), 0, 1); err != nil {
		t.Fatal(err)
	}
	encoded, err := json.Marshal(inbox.message)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(encoded), "secret-acquire-url") || strings.Contains(string(encoded), "secret-session-token") {
		t.Fatalf("secret persisted: %s", encoded)
	}
	if inbox.message.ScaleSetID != 42 || inbox.message.MessageID != 7 {
		t.Fatal("message identity lost")
	}
	if len(inbox.message.Available) != 1 {
		t.Fatal("available job lost")
	}
	job := inbox.message.Available[0]
	if job.Kind != scaleset.MessageTypeJobAvailable || job.WorkflowRef != "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev" || job.EventName != "push" {
		t.Fatalf("policy fields lost: %+v", job)
	}
	if len(job.Labels) != 1 || job.Labels[0] != "ra8ci-linux" {
		t.Fatalf("labels lost: %v", job.Labels)
	}
}

func TestGuardedClientRejectsMalformedMessage(t *testing.T) {
	client := testClient()
	client.message.Statistics = nil
	inbox := &fakeInbox{}
	guard, err := NewGuardedClient(client, inbox, 42)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := guard.GetMessage(context.Background(), 0, 1); err == nil {
		t.Fatal("malformed message accepted")
	}
	if inbox.saves != 0 {
		t.Fatal("malformed message persisted")
	}
}

func testClient() *fakeClient {
	return &fakeClient{
		session: scaleset.RunnerScaleSetSession{SessionID: uuid.New(), MessageQueueAccessToken: "secret-session-token", Statistics: &scaleset.RunnerScaleSetStatistic{TotalAssignedJobs: 1}},
		message: &scaleset.RunnerScaleSetMessage{
			MessageID:  7,
			Statistics: &scaleset.RunnerScaleSetStatistic{TotalAssignedJobs: 1},
			JobAvailableMessages: []*scaleset.JobAvailable{{
				AcquireJobURL: "secret-acquire-url",
				JobMessageBase: scaleset.JobMessageBase{
					JobMessageType:  scaleset.JobMessageType{MessageType: scaleset.MessageTypeJobAvailable},
					RunnerRequestID: 91,
					RepositoryName:  "ra8-firmware",
					OwnerName:       "bsikar",
					JobWorkflowRef:  "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev",
					EventName:       "push",
					RequestLabels:   []string{"ra8ci-linux"},
				},
			}},
		},
	}
}
