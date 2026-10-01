//go:build integration

package server

import (
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// What it takes to end an attempt.
//
// The result door is the last write a worker makes, and the only one that
// closes the attempt rather than adding to it. the_writes_a_superseded_worker_cannot_make_test.go
// holds the three doors that append evidence; this one holds the door that
// says the work is over, which has to be judged on the same fence as every
// write that came before it.

// receiptFor is a terminal receipt that passes every protocol rule, bound to
// this grant and to the host the attempt actually ran on, so a test can change
// exactly the one field it is about.
//
// It reports an attempt that failed before any step ran. That is the receipt
// with the fewest couplings to anything outside the grant: a succeeded receipt
// must name the catalog task's exact step list, which would make these tests
// fail whenever the fixture task's steps change, and the fence is what they
// are actually about.
func receiptFor(work agentWork, assignment protocol.Assignment) protocol.TerminalReceipt {
	started := time.Now().UTC()
	ended := started.Add(time.Second)
	// Both snapshots name one host and bracket the attempt, which is what the
	// receipt rules ask of a pair.
	atStart := work.facts
	atStart.CapturedAt = started
	atEnd := work.facts
	atEnd.CapturedAt = ended

	return protocol.TerminalReceipt{
		SchemaVersion:        protocol.Version,
		AssignmentID:         assignment.AssignmentID,
		AttemptID:            assignment.AttemptID,
		AssignmentVersion:    assignment.AssignmentVersion,
		FencingToken:         assignment.FencingToken,
		Outcome:              "failed",
		EvidenceComplete:     false,
		ErrorCode:            "no_step_executed",
		StartedAt:            started,
		EndedAt:              ended,
		DurationNS:           int64(time.Second),
		CatalogSHA256:        assignment.CatalogSHA256,
		SourceSnapshotSHA256: assignment.Source.SnapshotSHA256,
		HostFactsAtStart:     atStart,
		HostFactsAtEnd:       atEnd,
	}
}

func TestIntegrationAnAttemptIsCompletedOnlyOnItsOwnFence(t *testing.T) {
	work, assignment := running(t)
	path := "/v1/attempts/" + assignment.AttemptID + "/result"
	receipt := receiptFor(work, assignment)

	// The dead fence goes first deliberately: if ending the attempt were
	// judged any more loosely than appending to it, a superseded worker would
	// get to close work it no longer owns, and the refusal below would be the
	// only thing standing between that and a lost attempt.
	superseded := receipt
	superseded.FencingToken = assignment.FencingToken + 1
	refused := work.agentKnock(t, path, superseded)
	if refused.Code == http.StatusOK {
		t.Fatalf("a superseded worker completed the attempt: %s", refused.Body.String())
	}
	if refused.Code < 400 {
		t.Fatalf("a superseded completion answered %d, want a refusal", refused.Code)
	}

	accepted := work.agentKnock(t, path, receipt)
	if accepted.Code != http.StatusOK {
		t.Fatalf("a receipt on a live grant answered %d: %s", accepted.Code, accepted.Body.String())
	}
}

func TestIntegrationAReceiptNamingAnotherAttemptIsRefusedBeforeTheStore(t *testing.T) {
	work, assignment := running(t)
	receipt := receiptFor(work, assignment)

	// Well-formed, correctly fenced, and posted to a path it does not name.
	// The door settles this itself rather than letting the store decide which
	// of the two identities it should believe.
	result := work.agentKnock(t, "/v1/attempts/"+testAttemptID+"/result", receipt)
	if result.Code != http.StatusBadRequest {
		t.Fatalf("a receipt posted to another attempt answered %d: %s", result.Code, result.Body.String())
	}
}
