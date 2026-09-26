// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// gatedPlane refuses the logs endpoint until it is opened, so a test can make
// the uploader hold a chunk back and then let the last offer through.
type gatedPlane struct {
	open  atomic.Bool
	calls atomic.Int64
}

func (plane *gatedPlane) handler(assignment protocol.Assignment) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		plane.calls.Add(1)
		if !plane.open.Load() {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		acceptFor(w, assignment)
	}
}

// holdingUploader drives one failing write so the uploader ends up holding a
// chunk, and returns the uploader with the plane still shut.
func holdingUploader(t *testing.T, plane *gatedPlane, body []byte) (*logUploader, protocol.Assignment) {
	t.Helper()
	assignment := testAssignment()
	agent, server := testAgent(t, plane.handler(assignment))
	t.Cleanup(server.Close)
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
	if _, err := uploader.write("format-check", "stdout", body); err == nil {
		t.Fatal("a shut plane accepted the chunk")
	}
	if uploader.pending == nil {
		t.Fatal("a refused chunk was not held for the last offer")
	}
	return uploader, assignment
}

// TestRecoveredLogIsWholeOnlyWhenNothingWasDropped is the rule itself. A
// single byte turned away behind the held chunk is a log the plane cannot
// hold in full, however well the last offer goes.
func TestRecoveredLogIsWholeOnlyWhenNothingWasDropped(t *testing.T) {
	for _, dropped := range []int64{0} {
		if !recoveredLogIsWhole(dropped) {
			t.Fatalf("dropped %d read as an incomplete log", dropped)
		}
	}
	for _, dropped := range []int64{1, 2, 64, int64(protocol.MaxLogBytes), int64(protocol.MaxLogBytes) * 9} {
		if recoveredLogIsWhole(dropped) {
			t.Fatalf("dropped %d read as a whole log", dropped)
		}
	}
}

// The gap, at the uploader. The plane was briefly unreachable, the held chunk
// lands at the last offer, and the plane now holds every byte the attempt
// produced. Before this the sticky error stayed and the receipt reported a
// broken log for an attempt whose log was complete.
func TestAnAcceptedHeldChunkEndsTheFailureWhenTheLogIsWhole(t *testing.T) {
	plane := &gatedPlane{}
	uploader, _ := holdingUploader(t, plane, []byte("compiling\n"))
	if uploader.dropped != 0 {
		t.Fatalf("the held chunk itself was counted as dropped: %d", uploader.dropped)
	}
	plane.open.Store(true)

	uploader.flushGrace(context.Background())

	sequence, err := uploader.status()
	if sequence != 1 || err != nil {
		t.Fatalf("a recovered log still reports a failure: sequence=%d err=%v", sequence, err)
	}
	if uploader.pending != nil {
		t.Fatal("a chunk the plane accepted is still held")
	}
}

// The other half of the rule. Bytes refused behind the held chunk are gone
// whatever the last offer does, so the receipt has to keep saying the log is
// incomplete even though the final sequence moved.
func TestAnAcceptedHeldChunkKeepsTheFailureWhenBytesWereDropped(t *testing.T) {
	plane := &gatedPlane{}
	uploader, _ := holdingUploader(t, plane, []byte("compiling\n"))
	before := plane.calls.Load()
	if _, err := uploader.write("format-check", "stdout", []byte("linking\n")); err == nil {
		t.Fatal("a poisoned uploader accepted a later chunk")
	}
	if plane.calls.Load() != before {
		t.Fatal("a poisoned uploader spoke to the plane again")
	}
	if uploader.dropped != int64(len("linking\n")) {
		t.Fatalf("dropped = %d, want %d", uploader.dropped, len("linking\n"))
	}
	plane.open.Store(true)

	uploader.flushGrace(context.Background())

	sequence, err := uploader.status()
	if sequence != 1 {
		t.Fatalf("the accepted chunk did not move the final sequence: %d", sequence)
	}
	if err == nil {
		t.Fatal("a log missing the bytes behind the held chunk reported as whole")
	}
	if uploader.pending != nil {
		t.Fatal("a chunk the plane accepted is still held")
	}
}

// One byte is the whole separation. It is the difference between a receipt
// that says this attempt's log is complete and one that says it is not.
func TestOneDroppedByteIsEnoughToKeepTheFailure(t *testing.T) {
	plane := &gatedPlane{}
	uploader, _ := holdingUploader(t, plane, []byte("x"))
	if _, err := uploader.write("format-check", "stdout", []byte("y")); err == nil {
		t.Fatal("a poisoned uploader accepted a later chunk")
	}
	if uploader.dropped != 1 {
		t.Fatalf("dropped = %d, want 1", uploader.dropped)
	}
	plane.open.Store(true)
	uploader.flushGrace(context.Background())
	if _, err := uploader.status(); err == nil {
		t.Fatal("one dropped byte was forgiven")
	}
}

// The bytes left over inside the failing call are dropped too: they sit
// behind the held chunk and are never offered. Only they are counted, not the
// held chunk and not the chunks that already landed.
func TestBytesBehindTheHeldChunkInTheSameCallAreCounted(t *testing.T) {
	assignment := testAssignment()
	var calls atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		// Every offer of the second chunk is refused; the first and the
		// last offer of the second are accepted.
		if calls.Add(1) > 1 && calls.Load() <= int64(evidenceAttempts)+1 {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		acceptFor(w, assignment)
	})
	defer server.Close()
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}

	tail := 5
	data := []byte(strings.Repeat("x", protocol.MaxLogBytes*2+tail))
	written, err := uploader.write("step-one", "stdout", data)
	if written != protocol.MaxLogBytes || err == nil {
		t.Fatalf("partial upload = %d, %v", written, err)
	}
	if uploader.pending == nil || uploader.pending.Sequence != 2 {
		t.Fatalf("held chunk = %+v", uploader.pending)
	}
	if uploader.dropped != int64(tail) {
		t.Fatalf("dropped = %d, want %d (only the bytes behind the held chunk)", uploader.dropped, tail)
	}

	uploader.flushGrace(context.Background())

	sequence, status := uploader.status()
	if sequence != 2 {
		t.Fatalf("sequence = %d, want 2", sequence)
	}
	if status == nil {
		t.Fatal("a log missing its tail reported as whole")
	}
}

// A last offer the plane refuses changes nothing: the chunk stays held, the
// sequence stays where it was, and the failure stands.
func TestARefusedLastOfferLeavesTheFailureStanding(t *testing.T) {
	plane := &gatedPlane{}
	uploader, _ := holdingUploader(t, plane, []byte("compiling\n"))

	uploader.flushGrace(context.Background())

	sequence, err := uploader.status()
	if sequence != 0 || err == nil {
		t.Fatalf("a refused last offer moved the record: sequence=%d err=%v", sequence, err)
	}
	if uploader.pending == nil {
		t.Fatal("a chunk the plane never accepted was dropped")
	}
}

// acceptHeldChunk is only ever the answer to an accepted held chunk. With
// nothing held there is nothing to forgive, so an uploader that failed for
// some other reason keeps its error.
func TestAcceptHeldChunkWithNothingHeldForgivesNothing(t *testing.T) {
	uploader := &logUploader{sequence: 7, err: context.DeadlineExceeded}
	uploader.acceptHeldChunk()
	if uploader.sequence != 7 || uploader.err == nil {
		t.Fatalf("an uploader holding nothing was cleared: sequence=%d err=%v", uploader.sequence, uploader.err)
	}
}

// End to end at the receipt, which is where the gap was actually read. The
// same attempt, the same recovered chunk, and the two answers the plane is
// given about it.
func TestTheReceiptReportsARecoveredLogAsComplete(t *testing.T) {
	assignment := testAssignment()
	facts, err := HostFacts()
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now().UTC()
	result := executor.Result{TaskName: "format-check", StartedAt: now, EndedAt: now.Add(time.Second),
		Duration: time.Second, Steps: []executor.StepResult{{Name: "format-tree-check",
			StartedAt: now, EndedAt: now.Add(time.Second), Duration: time.Second}}}

	whole := &gatedPlane{}
	recovered, _ := holdingUploader(t, whole, []byte("compiling\n"))
	whole.open.Store(true)
	recovered.flushGrace(context.Background())
	sequence, logErr := recovered.status()
	receipt := terminalReceipt(assignment, result, facts, facts, sequence, nil, logErr, nil)
	if receipt.Outcome != "succeeded" || !receipt.EvidenceComplete || receipt.ErrorCode != "" ||
		receipt.FinalLogSequence != 1 || receipt.Validate() != nil {
		t.Fatalf("a passing attempt with a whole log was reported as %+v", receipt)
	}

	partial := &gatedPlane{}
	lossy, _ := holdingUploader(t, partial, []byte("compiling\n"))
	if _, err := lossy.write("format-check", "stdout", []byte("linking\n")); err == nil {
		t.Fatal("a poisoned uploader accepted a later chunk")
	}
	partial.open.Store(true)
	lossy.flushGrace(context.Background())
	sequence, logErr = lossy.status()
	receipt = terminalReceipt(assignment, result, facts, facts, sequence, nil, logErr, nil)
	if receipt.Outcome != "failed" || receipt.EvidenceComplete ||
		receipt.ErrorCode != "log_upload_error" || receipt.Validate() != nil {
		t.Fatalf("an attempt missing log bytes was reported as %+v", receipt)
	}
}
