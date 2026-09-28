// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/actions/scaleset"
	"github.com/google/uuid"
)

// What the listener hands this plane is a live scale-set response carrying an
// acquire URL and a session token. What goes in the inbox is the durable
// record, and the difference between the two is decided here, once, for all
// four job lists. guarded_test.go pins the available list; this pins the
// other three, the counts, and every shape the normalisation refuses.

// oneOfEach builds a response holding one job in each of the four lists, each
// with a secret the record must not carry.
func oneOfEach() *scaleset.RunnerScaleSetMessage {
	queued := time.Date(2026, 9, 28, 9, 0, 0, 0, time.UTC)
	base := func(kind scaleset.MessageType, request int64, name string) scaleset.JobMessageBase {
		return scaleset.JobMessageBase{
			JobMessageType:     scaleset.JobMessageType{MessageType: kind},
			RunnerRequestID:    request,
			RepositoryName:     "ra8-firmware",
			OwnerName:          "bsikar",
			JobID:              "job-" + name,
			JobWorkflowRef:     "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev",
			JobDisplayName:     name,
			WorkflowRunID:      4429117744,
			EventName:          "push",
			RequestLabels:      []string{"ra8ci-linux", "self-hosted"},
			QueueTime:          queued,
			ScaleSetAssignTime: queued.Add(time.Minute),
			RunnerAssignTime:   queued.Add(2 * time.Minute),
			FinishTime:         queued.Add(9 * time.Minute),
		}
	}
	return &scaleset.RunnerScaleSetMessage{
		MessageID: 11,
		Statistics: &scaleset.RunnerScaleSetStatistic{
			TotalAssignedJobs: 2, TotalRunningJobs: 3, TotalRegisteredRunners: 4,
			TotalBusyRunners: 5, TotalIdleRunners: 6, TotalAvailableJobs: 7, TotalAcquiredJobs: 8,
		},
		JobAvailableMessages: []*scaleset.JobAvailable{{
			AcquireJobURL:  "secret-acquire-url",
			JobMessageBase: base(scaleset.MessageTypeJobAvailable, 91, "build"),
		}},
		JobAssignedMessages: []*scaleset.JobAssigned{{
			JobMessageBase: base(scaleset.MessageTypeJobAssigned, 92, "lint"),
		}},
		JobStartedMessages: []*scaleset.JobStarted{{
			RunnerID: 17, RunnerName: "ra8ci-4429117744-2",
			JobMessageBase: base(scaleset.MessageTypeJobStarted, 93, "test"),
		}},
		JobCompletedMessages: []*scaleset.JobCompleted{{
			Result: "succeeded", RunnerID: 17, RunnerName: "ra8ci-4429117744-2",
			JobMessageBase: base(scaleset.MessageTypeJobCompleted, 94, "publish"),
		}},
	}
}

// recording runs one response through a guarded client and hands back what
// the inbox was given.
func recording(t *testing.T, src *scaleset.RunnerScaleSetMessage) (Message, error) {
	t.Helper()
	client := &fakeClient{
		session: scaleset.RunnerScaleSetSession{SessionID: uuid.New(), MessageQueueAccessToken: "secret-session-token"},
		message: src,
	}
	inbox := &fakeInbox{}
	guard, err := NewGuardedClient(client, inbox, 42)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := guard.GetMessage(context.Background(), 0, 1); err != nil {
		return Message{}, err
	}
	return inbox.message, nil
}

// Every list is carried across, and the two that name a runner carry the
// runner while the completed one also carries its result. A started job with
// no runner name would leave the plane unable to say which machine ran it.
func TestEveryJobListReachesTheRecordWithWhatItsListCarries(t *testing.T) {
	entry, err := recording(t, oneOfEach())
	if err != nil {
		t.Fatal(err)
	}
	if entry.ScaleSetID != 42 || entry.MessageID != 11 || entry.SessionID == "" {
		t.Fatalf("identity = %+v", entry)
	}
	if len(entry.Available) != 1 || len(entry.Assigned) != 1 || len(entry.Started) != 1 || len(entry.Completed) != 1 {
		t.Fatalf("lists = %d/%d/%d/%d", len(entry.Available), len(entry.Assigned), len(entry.Started), len(entry.Completed))
	}
	if entry.Available[0].Kind != scaleset.MessageTypeJobAvailable || entry.Available[0].RunnerRequestID != 91 {
		t.Fatalf("available = %+v", entry.Available[0])
	}
	if entry.Assigned[0].Kind != scaleset.MessageTypeJobAssigned || entry.Assigned[0].DisplayName != "lint" {
		t.Fatalf("assigned = %+v", entry.Assigned[0])
	}

	// An assigned job is not yet on a runner, so it names none.
	if entry.Assigned[0].RunnerID != 0 || entry.Assigned[0].RunnerName != "" || entry.Assigned[0].Result != "" {
		t.Fatalf("an assigned job named a runner or a result: %+v", entry.Assigned[0])
	}
	started := entry.Started[0]
	if started.RunnerID != 17 || started.RunnerName != "ra8ci-4429117744-2" || started.Result != "" {
		t.Fatalf("started = %+v", started)
	}
	done := entry.Completed[0]
	if done.RunnerID != 17 || done.RunnerName != "ra8ci-4429117744-2" || done.Result != "succeeded" {
		t.Fatalf("completed = %+v", done)
	}

	// The times a scheduling decision is judged against survive whole.
	if done.QueueTime.IsZero() || done.AssignTime.IsZero() || done.StartTime.IsZero() || done.FinishTime.IsZero() {
		t.Fatalf("a timestamp was dropped: %+v", done)
	}
	if !done.FinishTime.After(done.StartTime) || !done.StartTime.After(done.AssignTime) {
		t.Fatalf("the timestamps were shuffled: %+v", done)
	}
}

// The five counters this plane scales on are read off the named upstream
// fields, not off whichever happens to sit beside them.
func TestTheCapacitySnapshotIsReadFieldByField(t *testing.T) {
	entry, err := recording(t, oneOfEach())
	if err != nil {
		t.Fatal(err)
	}
	want := Statistics{Assigned: 2, Running: 3, Registered: 4, Busy: 5, Idle: 6}
	if entry.Statistics != want {
		t.Fatalf("statistics = %+v, want %+v", entry.Statistics, want)
	}
}

// The record is the thing that outlives the session, so no secret from any
// list may reach it, and the labels must be a copy: the upstream slice is
// the listener's and may be reused under it.
func TestTheRecordCarriesNoSecretAndNoSharedLabelSlice(t *testing.T) {
	src := oneOfEach()
	labels := src.JobAvailableMessages[0].RequestLabels

	entry, err := recording(t, src)
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := json.Marshal(entry)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(encoded), "secret-acquire-url") || strings.Contains(string(encoded), "secret-session-token") {
		t.Fatalf("a secret reached the record: %s", encoded)
	}

	labels[0] = "rewritten-under-the-record"
	if entry.Available[0].Labels[0] != "ra8ci-linux" {
		t.Fatal("the record shares its label slice with the listener")
	}
}

// A response this plane cannot record faithfully is refused before the inbox
// is asked, because a saved half-message would be acknowledged and lost.
func TestAResponseThatCannotBeRecordedIsRefusedBeforeTheInbox(t *testing.T) {
	noStatistics := oneOfEach()
	noStatistics.Statistics = nil

	unnumbered := oneOfEach()
	unnumbered.MessageID = 0

	negative := oneOfEach()
	negative.MessageID = -1

	holeInAvailable := oneOfEach()
	holeInAvailable.JobAvailableMessages = append(holeInAvailable.JobAvailableMessages, nil)

	holeInAssigned := oneOfEach()
	holeInAssigned.JobAssignedMessages = append(holeInAssigned.JobAssignedMessages, nil)

	holeInStarted := oneOfEach()
	holeInStarted.JobStartedMessages = append(holeInStarted.JobStartedMessages, nil)

	holeInCompleted := oneOfEach()
	holeInCompleted.JobCompletedMessages = append(holeInCompleted.JobCompletedMessages, nil)

	for name, src := range map[string]*scaleset.RunnerScaleSetMessage{
		"no statistics":     noStatistics,
		"no message number": unnumbered,
		"a negative number": negative,
		"a nil available":   holeInAvailable,
		"a nil assigned":    holeInAssigned,
		"a nil started":     holeInStarted,
		"a nil completed":   holeInCompleted,
	} {
		client := &fakeClient{
			session: scaleset.RunnerScaleSetSession{SessionID: uuid.New()},
			message: src,
		}
		inbox := &fakeInbox{}
		guard, err := NewGuardedClient(client, inbox, 42)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := guard.GetMessage(context.Background(), 0, 1); err == nil {
			t.Fatalf("%s was accepted", name)
		}
		if inbox.saves != 0 {
			t.Fatalf("%s reached the inbox", name)
		}
	}
}

// A response holding no jobs at all is ordinary: the queue is empty and the
// capacity snapshot is the whole point of the message.
func TestAnEmptyResponseIsRecordedRatherThanRefused(t *testing.T) {
	quiet := &scaleset.RunnerScaleSetMessage{
		MessageID:  12,
		Statistics: &scaleset.RunnerScaleSetStatistic{TotalIdleRunners: 3},
	}
	entry, err := recording(t, quiet)
	if err != nil {
		t.Fatal(err)
	}
	if entry.MessageID != 12 || entry.Statistics.Idle != 3 {
		t.Fatalf("entry = %+v", entry)
	}
	if entry.Available != nil || entry.Assigned != nil || entry.Started != nil || entry.Completed != nil {
		t.Fatalf("an empty response grew job lists: %+v", entry)
	}
}
