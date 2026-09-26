package server

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestMeasuredStepEvidenceIsAccepted(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	in, err := offlineInput(entry, cat)
	if err != nil {
		t.Fatalf("measured step evidence was refused: %v", err)
	}
	if len(in.Steps) != 1 || in.Steps[0].StdoutSHA256 != entry.Result.Steps[0].StdoutSHA256 {
		t.Fatalf("step evidence not carried through: %+v", in.Steps)
	}
}

func TestAStepDigestNoRunCouldHaveMeasuredIsRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, c := range []struct {
		name   string
		digest string
	}{
		{"empty", ""},
		{"not hex", strings.Repeat("z", 64)},
		{"uppercase", strings.ToUpper(strings.Repeat("a", 64))},
		{"too short", strings.Repeat("a", 63)},
		{"too long", strings.Repeat("a", 65)},
		{"a word", "unknown"},
	} {
		for _, stream := range []string{"stdout", "stderr"} {
			entry := stepped(t, func(e *spool.Entry) {
				if stream == "stdout" {
					e.Result.Steps[0].StdoutSHA256 = c.digest
					return
				}
				e.Result.Steps[0].StderrSHA256 = c.digest
			})
			in, err := offlineInput(entry, cat)
			if err == nil {
				t.Fatalf("%s %s digest was stored: %+v", c.name, stream, in.Steps)
			}
			if !errors.Is(err, store.ErrInvalid) {
				t.Fatalf("%s %s refusal does not travel as invalid: %v", c.name, stream, err)
			}
		}
	}
}

// TestASilentStepIsStillMeasured pins the case an empty-output rule would get
// wrong: a step that printed nothing still carries the digest of nothing, and
// the executor records exactly that.
func TestASilentStepIsStillMeasured(t *testing.T) {
	_, cat := offlineTestEntry(t)
	const emptySHA = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].StdoutSHA256 = emptySHA
		e.Result.Steps[0].StderrSHA256 = emptySHA
		e.Result.Steps[0].StdoutBytes = 0
		e.Result.Steps[0].StderrBytes = 0
	})
	if _, err := offlineInput(entry, cat); err != nil {
		t.Fatalf("a silent step was refused: %v", err)
	}
}

func TestANegativeStepByteCountIsRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, c := range []struct {
		name   string
		mutate func(*spool.Entry)
	}{
		{"stdout", func(e *spool.Entry) { e.Result.Steps[0].StdoutBytes = -1 }},
		{"stderr", func(e *spool.Entry) { e.Result.Steps[0].StderrBytes = -1 }},
	} {
		if in, err := offlineInput(stepped(t, c.mutate), cat); err == nil {
			t.Fatalf("a negative %s byte count was stored: %+v", c.name, in.Steps)
		}
	}
}

// TestAStepThatPrintedIsNotJudgedAgainstItsByteCount states a bound this rule
// deliberately does not have: the digest is over the bytes, but the count is
// not recomputable from a record carrying no logs, so a count that disagrees
// with the digest is not something this door can tell.
func TestAStepThatPrintedIsNotJudgedAgainstItsByteCount(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) { e.Result.Steps[0].StdoutBytes = 4096 })
	if _, err := offlineInput(entry, cat); err != nil {
		t.Fatalf("a stated byte count was judged against a digest it cannot be checked with: %v", err)
	}
}
