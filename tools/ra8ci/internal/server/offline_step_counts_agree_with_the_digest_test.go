package server

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// measuredStream states the pair a real run would state for the given output: the
// digest over exactly those bytes and their count, taken together the way
// digestWriter takes them.
func measuredStream(output string) (string, int64) {
	sum := sha256.Sum256([]byte(output))
	return hex.EncodeToString(sum[:]), int64(len(output))
}

func countsOf(t *testing.T, stdout, stderr string) spool.Entry {
	t.Helper()
	outDigest, outBytes := measuredStream(stdout)
	errDigest, errBytes := measuredStream(stderr)
	return stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].StdoutSHA256, e.Result.Steps[0].StdoutBytes = outDigest, outBytes
		e.Result.Steps[0].StderrSHA256, e.Result.Steps[0].StderrBytes = errDigest, errBytes
	})
}

func TestTheEmptyDigestIsSHA256OverNothing(t *testing.T) {
	digest, bytes := measuredStream("")
	if digest != digestOfNoBytes || bytes != 0 {
		t.Fatalf("the constant is not the digest of nothing: %s", digest)
	}
}

func TestCountsThatAgreeWithTheirDigestsAreAccepted(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, c := range []struct{ name, stdout, stderr string }{
		{"both silent", "", ""},
		{"stdout only", "ok\n", ""},
		{"stderr only", "", "warning\n"},
		{"both spoke", "ok\n", "warning\n"},
		{"a long line", strings.Repeat("x", 4096), "e"},
	} {
		if _, err := offlineInput(countsOf(t, c.stdout, c.stderr), cat); err != nil {
			t.Fatalf("%s was refused: %v", c.name, err)
		}
	}
}

func TestBytesBesideTheDigestOfNoneAreRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, stream := range []string{"stdout", "stderr"} {
		entry := stepped(t, func(e *spool.Entry) {
			e.Result.Steps[0].StdoutSHA256, e.Result.Steps[0].StdoutBytes = digestOfNoBytes, 0
			e.Result.Steps[0].StderrSHA256, e.Result.Steps[0].StderrBytes = digestOfNoBytes, 0
			if stream == "stdout" {
				e.Result.Steps[0].StdoutBytes = 4096
				return
			}
			e.Result.Steps[0].StderrBytes = 4096
		})
		in, err := offlineInput(entry, cat)
		if err == nil {
			t.Fatalf("%s bytes beside the empty digest were stored: %+v", stream, in.Steps)
		}
		if !errors.Is(err, store.ErrInvalid) {
			t.Fatalf("%s refusal does not travel as invalid: %v", stream, err)
		}
	}
}

func TestNoBytesBesideAMeasuredDigestIsRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	spoke, _ := measuredStream("ok\n")
	for _, stream := range []string{"stdout", "stderr"} {
		entry := stepped(t, func(e *spool.Entry) {
			e.Result.Steps[0].StdoutSHA256, e.Result.Steps[0].StdoutBytes = digestOfNoBytes, 0
			e.Result.Steps[0].StderrSHA256, e.Result.Steps[0].StderrBytes = digestOfNoBytes, 0
			if stream == "stdout" {
				e.Result.Steps[0].StdoutSHA256 = spoke
				return
			}
			e.Result.Steps[0].StderrSHA256 = spoke
		})
		in, err := offlineInput(entry, cat)
		if err == nil {
			t.Fatalf("a %s digest over bytes it never saw was stored: %+v", stream, in.Steps)
		}
		if !errors.Is(err, store.ErrInvalid) {
			t.Fatalf("%s refusal does not travel as invalid: %v", stream, err)
		}
	}
}

// TestASilentStepStillPasses pins the case this rule must not take away: the
// digest rule already accepts the digest of nothing, and a step that printed
// nothing states it beside a zero count.
func TestASilentStepStillPasses(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].StdoutSHA256, e.Result.Steps[0].StdoutBytes = digestOfNoBytes, 0
		e.Result.Steps[0].StderrSHA256, e.Result.Steps[0].StderrBytes = digestOfNoBytes, 0
	})
	if _, err := offlineInput(entry, cat); err != nil {
		t.Fatalf("a silent step was refused: %v", err)
	}
}

// TestTheShapeRuleStillRunsFirst pins the order: a digest that is not a digest
// is refused for its shape, not compared against the empty one.
func TestTheShapeRuleStillRunsFirst(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].StdoutSHA256, e.Result.Steps[0].StdoutBytes = "unknown", 0
	})
	_, err := offlineInput(entry, cat)
	if err == nil || !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("a shapeless digest was not refused as invalid: %v", err)
	}
	if !strings.Contains(err.Error(), "no run could have measured") {
		t.Fatalf("refused by the wrong rule: %v", err)
	}
}

func TestTheCountRuleNamesTheStepAndTheStream(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].StdoutSHA256, e.Result.Steps[0].StdoutBytes = digestOfNoBytes, 7
		e.Result.Steps[0].StderrSHA256, e.Result.Steps[0].StderrBytes = digestOfNoBytes, 0
	})
	_, err := offlineInput(entry, cat)
	if err == nil {
		t.Fatal("the mismatch was accepted")
	}
	if !strings.Contains(err.Error(), entry.Result.Steps[0].Name) || !strings.Contains(err.Error(), "stdout") {
		t.Fatalf("refusal does not name the step and stream: %v", err)
	}
}
