package syncclient

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// digestOfNothing is what the executor records for a step that printed
// nothing: SHA-256 over no bytes at all.
const digestOfNothing = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

// printing builds a terminal record with one step stating the given log
// evidence.
func printing(step executor.StepResult) spool.Entry {
	started := time.Date(2026, 9, 27, 14, 30, 0, 0, time.UTC)
	finished := started.Add(time.Minute)
	step.StartedAt, step.EndedAt = started, finished
	return spool.Entry{
		SchemaVersion: 2,
		ID:            strings.Repeat("e", 32),
		Task:          "unit-tests",
		StartedAt:     started,
		FinishedAt:    &finished,
		SyncState:     "unsynced",
		Result: &executor.Result{
			TaskName: "unit-tests", StartedAt: started, EndedAt: finished,
			Steps: []executor.StepResult{step},
		},
	}
}

func measured(name string) executor.StepResult {
	return executor.StepResult{
		Name:         name,
		StdoutSHA256: strings.Repeat("a1b2c3d4", 8),
		StderrSHA256: digestOfNothing,
		StdoutBytes:  4096,
	}
}

func TestEvidenceARunMeasuredIsUploaded(t *testing.T) {
	if err := checkUploadedLogEvidenceWasMeasured(printing(measured("go test"))); err != nil {
		t.Fatalf("measured log evidence was refused: %v", err)
	}
}

// A silent step is not an exception: the digest of nothing is still a digest,
// and that is what the executor records beside a zero count.
func TestASilentStepIsUploaded(t *testing.T) {
	step := measured("go vet")
	step.StdoutSHA256, step.StdoutBytes = digestOfNothing, 0
	if err := checkUploadedLogEvidenceWasMeasured(printing(step)); err != nil {
		t.Fatalf("a silent step was refused: %v", err)
	}
}

func TestADigestNoRunCouldHaveMeasuredIsRefused(t *testing.T) {
	for _, digest := range []string{"", "unknown", strings.Repeat("a", 63), strings.Repeat("a", 65),
		strings.ToUpper(digestOfNothing), strings.Repeat("g", 64)} {
		step := measured("go test")
		step.StdoutSHA256 = digest
		err := checkUploadedLogEvidenceWasMeasured(printing(step))
		if !errors.Is(err, ErrUnmeasuredLogEvidence) {
			t.Fatalf("stdout digest %q was not refused: %v", digest, err)
		}
		if !strings.Contains(err.Error(), "go test") {
			t.Fatalf("the refusal does not name the step: %v", err)
		}
	}
}

// Both streams are read, and the refusal says which one.
func TestAnUnmeasuredStderrDigestIsRefused(t *testing.T) {
	step := measured("go test")
	step.StderrSHA256 = "none"
	err := checkUploadedLogEvidenceWasMeasured(printing(step))
	if !errors.Is(err, ErrUnmeasuredLogEvidence) {
		t.Fatalf("an unmeasured stderr digest was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "stderr") {
		t.Fatalf("the refusal does not name the stream: %v", err)
	}
}

func TestANegativeByteCountIsRefused(t *testing.T) {
	for _, counts := range [][2]int64{{-1, 0}, {0, -1}} {
		step := measured("go test")
		step.StdoutBytes, step.StderrBytes = counts[0], counts[1]
		if err := checkUploadedLogEvidenceWasMeasured(printing(step)); !errors.Is(err, ErrUnmeasuredLogEvidence) {
			t.Fatalf("counts %v were not refused: %v", counts, err)
		}
	}
}

// Every step is read, not only the first.
func TestALaterStepStatingUnmeasuredEvidenceIsRefused(t *testing.T) {
	entry := printing(measured("go build"))
	bad := measured("go test")
	bad.StderrSHA256 = ""
	entry.Result.Steps = append(entry.Result.Steps, bad)
	err := checkUploadedLogEvidenceWasMeasured(entry)
	if !errors.Is(err, ErrUnmeasuredLogEvidence) {
		t.Fatalf("a later unmeasured step was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "go test") {
		t.Fatalf("the refusal names the wrong step: %v", err)
	}
}

// A count that disagrees with a MEASURED digest is not this door's question:
// the pair is the server's own rule, and restating half of it here would be a
// second opinion on a shape this door was not written to judge.
func TestACountDisagreeingWithTheDigestIsLeftToTheDoorThatJudgesIt(t *testing.T) {
	step := measured("go test")
	step.StdoutSHA256, step.StdoutBytes = digestOfNothing, 4096
	if err := checkUploadedLogEvidenceWasMeasured(printing(step)); err != nil {
		t.Fatalf("a disagreeing count was refused here: %v", err)
	}
}

func TestARecordWithNoResultIsLeftToTheDoorsThatJudgeItsRun(t *testing.T) {
	entry := printing(measured("go test"))
	entry.Result = nil
	if err := checkUploadedLogEvidenceWasMeasured(entry); err != nil {
		t.Fatalf("a record with no result was refused here: %v", err)
	}
}
