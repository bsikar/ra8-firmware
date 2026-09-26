package boardagent

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// A reason the record can already hold is handed back byte for byte. Nothing
// here is a sanitizer for ordinary text.
func TestAReasonTheRecordCanHoldIsUnchanged(t *testing.T) {
	for _, reason := range []string{
		"",
		"HIL attempt failed",
		"invalid persistent board agent configuration: pinned HIL timing differs from manifest",
		strings.Repeat("a", maxCompletionReason),
		strings.Repeat("\u00e9", maxCompletionReason/2),
		"flash restore for board EK-RA8D2 (fixture-v2) did not settle",
	} {
		if got := boundedReason(reason); got != reason {
			t.Fatalf("a storable reason was rewritten: %d bytes in, %d bytes out", len(reason), len(got))
		}
	}
}

// The bound is bytes, because the bound the store applies is bytes.
func TestAnOverlongReasonIsCutToTheBound(t *testing.T) {
	for _, row := range []struct {
		name  string
		input string
	}{
		{"ascii", strings.Repeat("a", maxCompletionReason+1)},
		{"ascii far over", strings.Repeat("a", 64*maxCompletionReason)},
		{"two byte runes", strings.Repeat("\u00e9", maxCompletionReason)},
		{"three byte runes", strings.Repeat("\u20ac", maxCompletionReason)},
		{"four byte runes", strings.Repeat("\U0001f9ea", maxCompletionReason)},
	} {
		got := boundedReason(row.input)
		if len(got) > maxCompletionReason {
			t.Fatalf("%s: cut to %d bytes, above the bound", row.name, len(got))
		}
		if !strings.HasPrefix(row.input, got) {
			t.Fatalf("%s: the cut is not a prefix of the reason", row.name)
		}
	}
}

// The whole point: the cut never lands inside a rune. Every offset at which a
// multi-byte rune can straddle the bound is built here deliberately.
func TestTheCutNeverLandsInsideARune(t *testing.T) {
	for _, glyph := range []string{"\u00e9", "\u20ac", "\U0001f9ea"} {
		for lead := 0; lead < 8; lead++ {
			input := strings.Repeat("a", maxCompletionReason-lead) + strings.Repeat(glyph, 8)
			got := boundedReason(input)
			if !utf8.ValidString(got) {
				t.Fatalf("lead %d with %q: the cut reason is not text", lead, glyph)
			}
			if len(got) > maxCompletionReason {
				t.Fatalf("lead %d with %q: cut to %d bytes", lead, glyph, len(got))
			}
		}
	}
}

// A cut costs at most the bytes of one straddling rune, so a reason is not
// quietly shortened to keep the arithmetic easy.
func TestTheCutKeepsEverythingThatFits(t *testing.T) {
	for _, glyph := range []string{"a", "\u00e9", "\u20ac", "\U0001f9ea"} {
		input := strings.Repeat(glyph, 2*maxCompletionReason)
		got := boundedReason(input)
		if len(got) < maxCompletionReason-(utf8.RuneLen([]rune(glyph)[0])-1) {
			t.Fatalf("%q: kept only %d of %d bytes", glyph, len(got), maxCompletionReason)
		}
	}
}

// Bytes that were never text are dropped here rather than left for the
// database to refuse, which is the same lost attempt by a longer route.
func TestBytesThatWereNeverTextAreDropped(t *testing.T) {
	for _, row := range []struct {
		name  string
		input string
	}{
		{"short", "step failed: \xff\xfe"},
		{"long", strings.Repeat("a", maxCompletionReason) + "\xff" + strings.Repeat("b", 64)},
		{"lone continuation byte", "\x80 after"},
		{"truncated rune at the end", "ends with " + "\u20ac"[:2]},
	} {
		got := boundedReason(row.input)
		if !utf8.ValidString(got) {
			t.Fatalf("%s: invalid bytes reached the record", row.name)
		}
		if len(got) > maxCompletionReason {
			t.Fatalf("%s: %d bytes", row.name, len(got))
		}
	}
}

// Both conditions hold over every length around the bound, for every rune
// width, with nothing special-cased.
func TestEveryLengthAroundTheBoundIsStorable(t *testing.T) {
	for _, glyph := range []string{"a", "\u00e9", "\u20ac", "\U0001f9ea"} {
		width := utf8.RuneLen([]rune(glyph)[0])
		for bytes := maxCompletionReason - 16; bytes <= maxCompletionReason+16; bytes++ {
			input := strings.Repeat(glyph, bytes/width) + strings.Repeat("a", bytes%width)
			got := boundedReason(input)
			if !utf8.ValidString(got) || len(got) > maxCompletionReason {
				t.Fatalf("%q at %d bytes: valid=%v len=%d", glyph, bytes, utf8.ValidString(got), len(got))
			}
		}
	}
}

// The store's own bound is bytes and this constant is what the cut is held
// to, so a reason that passes here is one validAttemptResult accepts.
func TestTheBoundIsTheOneTheStoreApplies(t *testing.T) {
	if maxCompletionReason != 1024 {
		t.Fatalf("the agent's reason bound drifted from the store's: %d", maxCompletionReason)
	}
}

// End to end: a step that fails with a long multi-byte error must still come
// back as a completion the record can take.
func TestALongMultiByteStepFailureIsStillRecordable(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	if err := catalog.ValidateTask(assignment.Task); err != nil {
		t.Fatalf("test task invalid: %v", err)
	}
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			return 3, fmt.Errorf("uart scrape mismatch: %s", strings.Repeat("\u20ac", maxCompletionReason))
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if completion.Result != "failed" {
		t.Fatalf("a failing step did not fail the attempt: %+v", completion)
	}
	if !utf8.ValidString(completion.Reason) {
		t.Fatalf("the recorded reason is not text")
	}
	if len(completion.Reason) > maxCompletionReason {
		t.Fatalf("the recorded reason is %d bytes", len(completion.Reason))
	}
	if !strings.HasPrefix(completion.Reason, "uart scrape mismatch:") {
		t.Fatalf("the cut lost what the failure was: %q", completion.Reason[:64])
	}
}

// An ordinary short failure is recorded whole, cut or no cut.
func TestAnOrdinaryFailureReasonIsRecordedWhole(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			return 0, errors.New("fixture cable is unseated")
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if !strings.Contains(completion.Reason, "fixture cable is unseated") {
		t.Fatalf("an ordinary reason did not reach the completion: %q", completion.Reason)
	}
}
