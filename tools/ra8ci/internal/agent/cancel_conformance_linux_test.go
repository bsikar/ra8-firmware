// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// TestFakePlaneCancelTearsDownTheStepBeforeTheReceipt drives the other half of
// the protocol: the plane asks for its runner back mid-attempt.
//
// The property under test is an ordering one, and ordering is what a test run
// after the fact cannot see. A step process found dead once RunOnce returned
// proves only that it died eventually. So the plane itself looks, at the
// instant the terminal receipt arrives, at whether the step's process is
// still alive, and records a violation if it is. The receipt is then evidence
// about a guest that has actually been given back, not a promise.
func TestFakePlaneCancelTearsDownTheStepBeforeTheReceipt(t *testing.T) {
	pidPath := filepath.Join(t.TempDir(), "step.pid")
	// The step announces itself, records its own pid, then outlives the
	// test by a wide margin: nothing but the cancel can end it in time.
	root, snapshot := fixtureCheckout(t, fmt.Sprintf(
		"printf 'agent-log\\n'\necho $$ > %q\nsleep 300\n", pidPath))
	assignment := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	assignment.CatalogSHA256 = definitions.Digest()
	assignment.Source.Commit, assignment.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest

	plane := newFakePlane(assignment)
	plane.cancelNextHeartbeat()
	plane.atReceipt = func() string { return stepStillAlive(pidPath) }
	agent, closeServer := planeFixture(t, plane)
	defer closeServer()
	agent.root = root
	// The cancel only reaches a running attempt on a heartbeat, so beat
	// fast enough to observe the teardown inside a test.
	agent.beat = 200 * time.Millisecond

	started := time.Now()
	assigned, err := agent.RunOnce(context.Background())
	elapsed := time.Since(started)
	if !assigned {
		t.Fatalf("claim = %v, %v", assigned, err)
	}
	if elapsed > 30*time.Second {
		t.Fatalf("attempt ran %v: the cancel did not end the step", elapsed)
	}

	_, logs, receipt, violations := plane.state()
	if len(violations) != 0 {
		t.Fatalf("the agent broke the contract under cancellation: %v", violations)
	}
	if receipt == nil {
		t.Fatal("a cancelled attempt still owes the plane a terminal receipt")
	}
	if receipt.Outcome != "cancelled" || !receipt.Cancelled || receipt.TimedOut {
		t.Fatalf("receipt does not report the cancellation: %+v", receipt)
	}
	// Cancellation is not a failure of the evidence: the plane asked for
	// the runner back and got a complete account of what ran before it.
	if !receipt.EvidenceComplete || receipt.ErrorCode != "" {
		t.Fatalf("cancellation degraded the evidence: %+v", receipt)
	}
	if receipt.FinalLogSequence != int64(logs) {
		t.Fatalf("receipt claims %d log chunks, plane holds %d", receipt.FinalLogSequence, logs)
	}
	if len(receipt.Steps) != 1 || !receipt.Steps[0].Cancelled || receipt.Steps[0].TimedOut {
		t.Fatalf("step summary does not report the cancellation: %+v", receipt.Steps)
	}
	if plane.beats() == 0 {
		t.Fatal("no heartbeat arrived, so the cancel was never carried")
	}
	// A cancelled attempt hands the runner back rather than spending the
	// fence window uploading files nobody asked for.
	if chunks, manifests := plane.artifacts(); chunks != 0 || manifests != 0 {
		t.Fatalf("cancelled attempt uploaded %d chunks and %d manifests", chunks, manifests)
	}
}

// TestCancelledAttemptUploadsNoArtifactsThePlaneWouldTake pins the #1513
// judgement call against a party that would have accepted the upload. The
// existing coverage asserts against a recorder that records whatever arrives;
// here the same decision is made in front of the enforcing plane, and the
// companion case proves the plane really does take artifacts when an attempt
// is entitled to send them. Without that half, "nothing arrived" would be
// indistinguishable from a plane that refuses artifacts outright.
func TestCancelledAttemptUploadsNoArtifactsThePlaneWouldTake(t *testing.T) {
	for _, attempt := range []struct {
		name      string
		cancelled bool
		chunks    int
	}{
		{"cancelled", true, 0},
		{"completed", false, 1},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			assignment := testAssignment()
			plane := newFakePlane(assignment)
			// The attempt is past its ack by the time outputs are read.
			plane.acknowledge()
			agent, closeServer := planeFixture(t, plane)
			defer closeServer()
			agent.root = checkoutWithOutput(t, "produced evidence\n")

			result := producedResult("compile")
			result.Cancelled = attempt.cancelled
			manifests, err := agent.collectAttemptArtifacts(context.Background(),
				assignment, outputTask(), result, artifactClock())
			if err != nil {
				t.Fatalf("collect = %v", err)
			}
			if len(manifests) != attempt.chunks {
				t.Fatalf("uploader reports %d manifests, want %d", len(manifests), attempt.chunks)
			}
			chunks, closed := plane.artifacts()
			if chunks != attempt.chunks || closed != attempt.chunks {
				t.Fatalf("plane holds %d chunks and %d manifests, want %d of each",
					chunks, closed, attempt.chunks)
			}
			if _, _, _, violations := plane.state(); len(violations) != 0 {
				t.Fatalf("artifact upload broke the contract: %v", violations)
			}
		})
	}
}

// checkoutWithOutput is a root holding the one declared output of outputTask.
func checkoutWithOutput(t *testing.T, body string) string {
	t.Helper()
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "out"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "out", "report.txt"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return root
}

// stepStillAlive answers whether the process the step recorded is still
// running. An unreadable pid file is itself an answer: the test cannot claim
// the teardown happened, so it says so rather than passing on silence.
func stepStillAlive(pidPath string) string {
	data, err := os.ReadFile(pidPath)
	if err != nil {
		return "step pid file unreadable at the receipt: " + err.Error()
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil || pid <= 1 {
		return "step pid file does not name a process: " + strings.TrimSpace(string(data))
	}
	// Signal 0 checks for existence only. The executor waits on the child,
	// so a reaped process is gone rather than a zombie that still answers.
	if err := syscall.Kill(pid, 0); err == nil {
		return fmt.Sprintf("step process %d was still alive when the terminal receipt arrived", pid)
	}
	return ""
}
