// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func enveloped(started time.Time, finished *time.Time) spool.Entry {
	return spool.Entry{ID: "local-1", StartedAt: started, FinishedAt: finished}
}

func at(value time.Time) *time.Time { return &value }

func TestEnvelopeOfAnOrdinaryRunIsRead(t *testing.T) {
	start := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	if err := checkUploadedEnvelopeIsReadable(enveloped(start, at(start.Add(11*time.Minute)))); err != nil {
		t.Fatalf("ordinary envelope refused: %v", err)
	}
}

func TestEnvelopeOfAZeroLengthRunIsRead(t *testing.T) {
	start := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	if err := checkUploadedEnvelopeIsReadable(enveloped(start, at(start))); err != nil {
		t.Fatalf("a run that began and finished in the same instant was refused: %v", err)
	}
}

func TestEnvelopeWithNoFinishStampIsRefused(t *testing.T) {
	start := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	err := checkUploadedEnvelopeIsReadable(enveloped(start, nil))
	if !errors.Is(err, ErrUnreadableEnvelope) {
		t.Fatalf("absent finish stamp = %v, want ErrUnreadableEnvelope", err)
	}
}

func TestEnvelopeWithAZeroFinishStampIsRefused(t *testing.T) {
	start := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	err := checkUploadedEnvelopeIsReadable(enveloped(start, at(time.Time{})))
	if !errors.Is(err, ErrUnreadableEnvelope) {
		t.Fatalf("zero finish stamp = %v, want ErrUnreadableEnvelope", err)
	}
}

// The shape spool.Pending lets through: the order rule is satisfied by any
// real finish, because every time is after the zero time.
func TestEnvelopeWithAZeroStartStampIsRefused(t *testing.T) {
	finish := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	err := checkUploadedEnvelopeIsReadable(enveloped(time.Time{}, at(finish)))
	if !errors.Is(err, ErrUnreadableEnvelope) {
		t.Fatalf("zero start stamp = %v, want ErrUnreadableEnvelope", err)
	}
}

func TestEnvelopeWithBothStampsZeroIsRefused(t *testing.T) {
	err := checkUploadedEnvelopeIsReadable(enveloped(time.Time{}, at(time.Time{})))
	if !errors.Is(err, ErrUnreadableEnvelope) {
		t.Fatalf("two zero stamps = %v, want ErrUnreadableEnvelope", err)
	}
}

func TestEnvelopeAtTheServersBoundIsRead(t *testing.T) {
	start := time.Date(2026, 9, 26, 4, 0, 0, 0, time.UTC)
	if err := checkUploadedEnvelopeIsReadable(enveloped(start, at(start.Add(maxUploadedEnvelope)))); err != nil {
		t.Fatalf("envelope of exactly %s refused: %v", maxUploadedEnvelope, err)
	}
}

func TestEnvelopeOneNanosecondOverTheBoundIsRefused(t *testing.T) {
	start := time.Date(2026, 9, 26, 4, 0, 0, 0, time.UTC)
	err := checkUploadedEnvelopeIsReadable(enveloped(start, at(start.Add(maxUploadedEnvelope+1))))
	if !errors.Is(err, ErrUnreadableEnvelope) {
		t.Fatalf("envelope one nanosecond over the bound = %v, want ErrUnreadableEnvelope", err)
	}
}

// A host that sat offline for a week, or whose clock stepped forward between
// Begin and Finish, states stamps that are in order the whole way.
func TestEnvelopeSpanningDaysIsRefused(t *testing.T) {
	start := time.Date(2026, 9, 20, 4, 0, 0, 0, time.UTC)
	err := checkUploadedEnvelopeIsReadable(enveloped(start, at(start.Add(7*24*time.Hour))))
	if !errors.Is(err, ErrUnreadableEnvelope) {
		t.Fatalf("week-long envelope = %v, want ErrUnreadableEnvelope", err)
	}
}

func TestRefusalNamesTheStampThatIsMissing(t *testing.T) {
	finish := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	noStart := checkUploadedEnvelopeIsReadable(enveloped(time.Time{}, at(finish)))
	noFinish := checkUploadedEnvelopeIsReadable(enveloped(finish, nil))
	if noStart == nil || noFinish == nil || noStart.Error() == noFinish.Error() {
		t.Fatalf("missing start and missing finish read alike: %v / %v", noStart, noFinish)
	}
}

func TestRefusalNamesTheSpanThatWasStated(t *testing.T) {
	start := time.Date(2026, 9, 20, 4, 0, 0, 0, time.UTC)
	err := checkUploadedEnvelopeIsReadable(enveloped(start, at(start.Add(48*time.Hour))))
	if err == nil {
		t.Fatal("two-day envelope was read")
	}
	if want := "48h0m0s"; !contains(err.Error(), want) {
		t.Fatalf("refusal %q does not name the span %q", err.Error(), want)
	}
}

// The rule reads the record alone. The same envelope is judged the same way
// whichever year the host believes it is, because this host's clock is the one
// under suspicion.
func TestEnvelopeIsJudgedWithoutReadingTheClockNow(t *testing.T) {
	long_ago := time.Date(1999, 1, 1, 0, 0, 0, 0, time.UTC)
	far_ahead := time.Date(2099, 1, 1, 0, 0, 0, 0, time.UTC)
	for _, start := range []time.Time{long_ago, far_ahead} {
		if err := checkUploadedEnvelopeIsReadable(enveloped(start, at(start.Add(time.Minute)))); err != nil {
			t.Fatalf("a one-minute envelope starting %s was refused: %v", start, err)
		}
	}
}

func TestEnvelopeRuleIgnoresEverythingElseOnTheRecord(t *testing.T) {
	start := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	entry := enveloped(start, at(start.Add(time.Second)))
	entry.SchemaVersion = 7
	entry.SyncState = "synced"
	entry.Source = spool.SourceIdentity{}
	if err := checkUploadedEnvelopeIsReadable(entry); err != nil {
		t.Fatalf("a readable envelope was refused for a field this rule does not judge: %v", err)
	}
}

func contains(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}
