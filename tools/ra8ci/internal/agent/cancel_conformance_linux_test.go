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
		"echo $$ > %q\nprintf 'agent-log\\n'\nsleep 60\n", pidPath))
	assignment := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	assignment.CatalogSHA256 = definitions.Digest()
	assignment.Source.Commit, assignment.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest

	plane := newFakePlane(assignment)
	plane.atReceipt = func() string { return stepStillAlive(pidPath) }
	agent, closeServer := planeFixture(t, plane)
	defer closeServer()
	agent.root = root
	// The cancel only reaches a running attempt on a heartbeat, so beat
	// fast enough to observe the teardown inside a test.
	agent.beat = 200 * time.Millisecond

	// A teardown can only be observed once there is something to tear down.
	// Armed before the attempt starts, the cancel races the step's own
	// startup, and on a loaded host it wins: the attempt ends having run
	// nothing, and the receipt hook reports a pid file that was never
	// written as though the agent had left a process behind.
	//
	// The step's own pid file is not the signal to wait on. Measured on a
	// loaded two-core host, the step stamps that file 313ms into the
	// attempt and this process cannot read it for a further twenty
	// seconds, until the step exits: the wait then expires against a step
	// that has already ended, and the case fails for a reason that is not
	// the property under test. A log chunk the plane has answered is the
	// same proof that the step is running, arrives in this process, and
	// cannot be held back by a filesystem view. The fixture writes the pid
	// before it writes that line, so a chunk here means the pid file is
	// written too, and it is read only at the receipt, by which point the
	// step has exited and it is readable.
	//
	// When this case does fail it fails slowly, and a bare elapsed time
	// cannot tell the two reasons apart: a cancel that was armed promptly
	// and did not end the step is a defect in the teardown, while a cancel
	// armed at the far end of the wait is a loaded host starving the
	// fixture and says nothing about the teardown at all. So the arming is
	// timed here and reported with any failure, which is the evidence the
	// next run needs and cannot recover after the fact.
	started := time.Now()
	recorded := make(chan cancelArming, 1)
	go func() {
		for deadline := started.Add(30 * time.Second); time.Now().Before(deadline); {
			if _, logs, _, _ := plane.state(); logs > 0 {
				plane.cancelNextHeartbeat()
				recorded <- cancelArming{sawOutput: true,
					armedAt: time.Since(started), beats: plane.beats()}
				return
			}
			time.Sleep(2 * time.Millisecond)
		}
		// Armed regardless, so a step whose output never arrives ends the
		// attempt with a clear failure rather than a long wait.
		plane.cancelNextHeartbeat()
		recorded <- cancelArming{armedAt: time.Since(started), beats: plane.beats()}
	}()

	assigned, err := agent.RunOnce(context.Background())
	elapsed := time.Since(started)
	arming := <-recorded
	if !assigned {
		t.Fatalf("claim = %v, %v", assigned, err)
	}
	// What the arming saw is judged before the clock, because the elapsed
	// time cannot tell the two failures apart and the clock reading is the
	// more alarming of the two. A cancel armed against a step that was
	// running and did not end it is a teardown defect. A cancel armed
	// against a step whose output never arrived says nothing about the
	// teardown at all, and the attempt then runs to the step's own sleep,
	// so the elapsed check fires first and reports a defect that was never
	// observed. Under coverage instrumentation on this box that is exactly
	// what happens: the run takes three times as long, no chunk reaches the
	// plane inside the wait, and the case blames a teardown it never tested.
	if !arming.sawOutput {
		// No heartbeat at all means the attempt never got as far as
		// carrying a cancel, so the host starved the fixture rather
		// than the agent leaving a process behind. The property is
		// untested here, which is not the same as failed, and
		// reporting it as a teardown defect would be a false alarm
		// standing in front of the real one.
		if arming.beats == 0 {
			t.Skipf("the host starved the fixture: the attempt never heartbeat, "+
				"so no cancel was ever carried and the teardown was not exercised (%s, attempt ran %v)",
				arming, elapsed)
		}
		t.Fatalf("no step output reached the plane, so the cancel never had a running step to end (%s)", arming)
	}
	if elapsed > 30*time.Second {
		t.Fatalf("attempt ran %v: the cancel did not end the step (%s)", elapsed, arming)
	}

	_, logs, receipt, violations := plane.state()
	if len(violations) != 0 {
		t.Fatalf("the agent broke the contract under cancellation: %v (%s)", violations, arming)
	}
	if receipt == nil {
		t.Fatalf("a cancelled attempt still owes the plane a terminal receipt (%s)", arming)
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
		t.Fatalf("no heartbeat arrived, so the cancel was never carried (%s)", arming)
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

// cancelArming is what the cancel's own arming leaves behind for a failure to
// quote: whether the step had announced itself first, how far into the attempt
// the plane was told to ask for its runner back, and the beat the plane
// answered it on. A failure that quotes this can be read; one that reports
// only an elapsed time cannot.
type cancelArming struct {
	sawOutput bool
	armedAt   time.Duration
	beats     int
}

func (a cancelArming) String() string {
	output := "no step output yet"
	if a.sawOutput {
		output = "step output had reached the plane"
	}
	return fmt.Sprintf("cancel armed %v into the attempt after %d beats, %s",
		a.armedAt.Round(time.Millisecond), a.beats, output)
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
