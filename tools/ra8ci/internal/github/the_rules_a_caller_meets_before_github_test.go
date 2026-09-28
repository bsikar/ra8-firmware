// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// The rules below are the ones a caller meets before anything reaches
// GitHub: what a check run's body may hold, what a demand source calls
// itself, what an inbox will accept, and which dependencies a guarded
// client refuses to exist without.

// The publishable-output rule is exported so a caller assembling a
// document of runs can refuse the whole document before the first run of
// it is posted, and a check run cannot be taken back once it is. The
// exported door must therefore answer exactly as the internal rule does.
func TestThePublishableOutputRuleIsOneDefinition(t *testing.T) {
	for name, output := range map[string]struct{ title, summary string }{
		"no summary":       {"title", ""},
		"summary too long": {"title", strings.Repeat("s", maxCheckRunSummary+1)},
		"title too long":   {strings.Repeat("t", maxCheckRunTitle+1), "summary"},
	} {
		err := CheckPublishableOutput(output.title, output.summary)
		if err == nil || !errors.Is(err, ErrCheckRunOutputUnusable) {
			t.Fatalf("%s answered %v", name, err)
		}
		if internal := checkPublishableOutput(output.title, output.summary); internal == nil ||
			internal.Error() != err.Error() {
			t.Fatalf("%s: exported %v, internal %v", name, err, internal)
		}
	}

	// A refusal says what was counted, so the caller knows what to drop.
	err := CheckPublishableOutput("title", strings.Repeat("s", maxCheckRunSummary+1))
	if !strings.Contains(err.Error(), "65536 characters") {
		t.Fatalf("the refusal did not name the count: %v", err)
	}

	for name, output := range map[string]struct{ title, summary string }{
		"at both ceilings": {strings.Repeat("t", maxCheckRunTitle), strings.Repeat("s", maxCheckRunSummary)},
		"no title at all":  {"", "summary"},
	} {
		if err := CheckPublishableOutput(output.title, output.summary); err != nil {
			t.Fatalf("%s was refused: %v", name, err)
		}
	}
}

// Both ceilings are counted in characters, the unit GitHub states them in.
// A byte count would refuse a legal summary written in a script whose
// characters take more than one byte.
func TestAnOutputIsCountedInCharactersNotBytes(t *testing.T) {
	summary := strings.Repeat("\u3042", maxCheckRunSummary)
	if len(summary) <= maxCheckRunSummary {
		t.Fatalf("fixture is not multi-byte: %d bytes", len(summary))
	}
	if err := CheckPublishableOutput("", summary); err != nil {
		t.Fatalf("a legal summary in a multi-byte script was refused: %v", err)
	}
	if err := CheckPublishableOutput("", summary+"\u3042"); err == nil {
		t.Fatal("one character past the ceiling was accepted")
	}
}

// The evidence a source writes is named by the adapter it is gated on, so
// a recorded unit of demand can be traced to the path that observed it.
func TestAScaleSetSourceNamesTheEvidenceItWrites(t *testing.T) {
	source, _ := enabledScaleSetSource(t, &metadataStub{meta: scaleSetMetadata()})
	if source.Adapter() != demand.ScaleSetAdapter {
		t.Fatalf("adapter = %q, want %q", source.Adapter(), demand.ScaleSetAdapter)
	}

	var absent *ScaleSetSource
	if absent.Enabled() {
		t.Fatal("a source that does not exist reported itself enabled")
	}
	if _, err := absent.Observe(context.Background(), Message{}); err == nil ||
		!errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("a source that does not exist observed: %v", err)
	}
	var missing context.Context
	if _, err := source.Observe(missing, Message{}); err == nil || !errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("an observation with no caller answered %v", err)
	}
}

// An inbox is what lets a restart replay what it already took from GitHub,
// so one built without a backend, or pointed at another scale set, would
// lose or mix deliveries.
func TestAnInboxThatCouldLoseADeliveryIsRefused(t *testing.T) {
	for name, built := range map[string]struct {
		backend    InboxStore
		scaleSetID int
	}{
		"no backend":         {nil, 42},
		"no scale set":       {&fakeInboxStore{}, 0},
		"negative scale set": {&fakeInboxStore{}, -1},
	} {
		if _, err := NewStoreInbox(built.backend, built.scaleSetID); err == nil {
			t.Fatalf("%s was built", name)
		}
	}

	inbox, err := NewStoreInbox(&fakeInboxStore{}, 42)
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	for name, message := range map[string]Message{
		"another scale set": {ScaleSetID: 43, SessionID: "s", MessageID: 1},
		"no session":        {ScaleSetID: 42, SessionID: "", MessageID: 1},
		"no message":        {ScaleSetID: 42, SessionID: "s", MessageID: 0},
		"negative message":  {ScaleSetID: 42, SessionID: "s", MessageID: -1},
	} {
		if err := inbox.Save(ctx, message); err == nil {
			t.Fatalf("Save took %s", name)
		}
		if err := inbox.MarkProcessed(ctx, message); err == nil {
			t.Fatalf("MarkProcessed took %s", name)
		}
	}

	var absent *StoreInbox
	if err := absent.Save(ctx, Message{}); err == nil {
		t.Fatal("an inbox that does not exist saved a message")
	}
	if _, err := absent.Pending(ctx, 1); err == nil {
		t.Fatal("an inbox that does not exist answered a page")
	}
	for _, limit := range []int{0, -1, 1001} {
		if _, err := inbox.Pending(ctx, limit); err == nil {
			t.Fatalf("a page of %d was taken", limit)
		}
	}
}

// One unit of demand gets one runner name, derived from the demand
// identity and nothing else, so a retried delivery or a reconciliation
// pass asks GitHub a lookup rather than a search.
func TestARunnerNameIsDerivedFromTheDemandIdentityAlone(t *testing.T) {
	event := demandFixture(demand.PhaseQueued)
	first, err := DemandRunnerName(event)
	if err != nil {
		t.Fatal(err)
	}
	again, err := DemandRunnerName(event)
	if err != nil || again != first {
		t.Fatalf("the same demand answered %q then %q (%v)", first, again, err)
	}
	if !strings.HasSuffix(first, "-2") || !strings.Contains(first, "4429117744") {
		t.Fatalf("the name does not carry the job and attempt: %q", first)
	}
	other := event
	other.RunAttempt = 3
	if third, _ := DemandRunnerName(other); third == first {
		t.Fatalf("a second attempt reused the first attempt's name %q", first)
	}
	noisy := event
	noisy.DeliveryID, noisy.CommitSHA, noisy.JobName = "d-99", strings.Repeat("f", 40), "other"
	if quiet, _ := DemandRunnerName(noisy); quiet != first {
		t.Fatalf("the name moved with something other than the identity: %q vs %q", quiet, first)
	}

	for name, broken := range map[string]demand.Event{
		"no job":           {JobID: 0, RunAttempt: 1},
		"negative job":     {JobID: -1, RunAttempt: 1},
		"no attempt":       {JobID: 7, RunAttempt: 0},
		"negative attempt": {JobID: 7, RunAttempt: -1},
	} {
		if _, err := DemandRunnerName(broken); err == nil || !errors.Is(err, demand.ErrInvalid) {
			t.Fatalf("%s answered %v", name, err)
		}
	}
}

// A lookup with nothing to look with is refused rather than asked of the
// forge, which spends a rate limit to answer it.
func TestARunnerLookupWithNothingToLookWithIsRefused(t *testing.T) {
	registrar, provider := registrarFixture(t)
	var absent *DemandRegistrar
	if _, _, err := absent.Registered(context.Background(), demandFixture(demand.PhaseQueued)); err == nil {
		t.Fatal("a registrar that does not exist answered a lookup")
	}
	var missing context.Context
	if _, _, err := registrar.Registered(missing, demandFixture(demand.PhaseQueued)); err == nil {
		t.Fatal("a lookup with no caller was made")
	}
	if _, _, err := registrar.Registered(context.Background(), demand.Event{}); err == nil {
		t.Fatal("a lookup for an unnamed demand was made")
	}
	if len(provider.minted) != 0 {
		t.Fatalf("a refused lookup minted %v", provider.minted)
	}
}

// A guarded client is what commits a message before the listener sees it,
// so one missing any of its three dependencies must not exist at all.
func TestAGuardedClientWithoutItsDependenciesIsNotBuilt(t *testing.T) {
	for name, built := range map[string]struct {
		inner      *fakeClient
		inbox      Inbox
		scaleSetID int
	}{
		"no client":    {nil, &fakeInbox{}, 42},
		"no inbox":     {testClient(), nil, 42},
		"no scale set": {testClient(), &fakeInbox{}, 0},
		"negative set": {testClient(), &fakeInbox{}, -1},
	} {
		var inner = built.inner
		if inner == nil {
			if _, err := NewGuardedClient(nil, built.inbox, built.scaleSetID); err == nil {
				t.Fatalf("%s was built", name)
			}
			continue
		}
		if _, err := NewGuardedClient(inner, built.inbox, built.scaleSetID); err == nil {
			t.Fatalf("%s was built", name)
		}
	}
}
