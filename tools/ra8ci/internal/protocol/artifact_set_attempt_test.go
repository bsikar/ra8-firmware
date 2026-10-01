// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
)

// otherGrantManifest is a manifest for a different artifact of the same
// attempt, so a set built from it and the sample differs in nothing but the
// field the test under it changes.
func otherGrantManifest() ArtifactManifest {
	manifest := sampleArtifactManifest()
	manifest.Path = "logs/hil/segment-0001.json"
	return manifest
}

func TestASetNamingOneGrantIsAccepted(t *testing.T) {
	if err := ValidateArtifactSet([]ArtifactManifest{sampleArtifactManifest(), otherGrantManifest()}); err != nil {
		t.Fatalf("two artifacts of one attempt are the ordinary case: %v", err)
	}
}

func TestASetIsRefusedWhenAnyGrantFieldDisagrees(t *testing.T) {
	cases := []struct {
		name  string
		apply func(*ArtifactManifest)
	}{
		{"assignment", func(m *ArtifactManifest) { m.AssignmentID = "018f3a2b-7c41-7d8e-8f90-a1b2c3d4e5f6" }},
		{"attempt", func(m *ArtifactManifest) { m.AttemptID = "018f3a2b-7c41-7d8e-8f90-a1b2c3d4e5f6" }},
		{"version", func(m *ArtifactManifest) { m.AssignmentVersion++ }},
		{"fence", func(m *ArtifactManifest) { m.FencingToken++ }},
	}
	for _, testCase := range cases {
		other := otherGrantManifest()
		testCase.apply(&other)
		if err := other.Validate(); err != nil {
			t.Fatalf("%s: the entry must be valid on its own or the test proves nothing: %v", testCase.name, err)
		}
		err := ValidateArtifactSet([]ArtifactManifest{sampleArtifactManifest(), other})
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s: a set naming two grants was accepted: %v", testCase.name, err)
		}
		if !strings.Contains(err.Error(), "more than one grant") {
			t.Fatalf("%s: refused for the wrong reason: %v", testCase.name, err)
		}
	}
}

// The order matters: a budget is meaningless until the denominator is known,
// so a set that both names two grants and exceeds the byte total must report
// the grant, not the budget. The bare ErrInvalid the budget returns carries no
// message, which is what tells the two apart.
func TestTheGrantIsJudgedBeforeTheBudget(t *testing.T) {
	first := sampleArtifactManifest()
	first.TotalBytes = MaxArtifactBytes
	first.FinalSequence = MinArtifactChunks(first.TotalBytes)
	second := first
	second.Path = "logs/second.bin"
	second.AttemptID = "018f3a2b-7c41-7d8e-8f90-a1b2c3d4e5f6"
	err := ValidateArtifactSet([]ArtifactManifest{first, second})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("want refusal, got %v", err)
	}
	if !strings.Contains(err.Error(), "more than one grant") {
		t.Fatalf("the budget preempted the grant: %v", err)
	}
}

// Keyed on the path alone, the duplicate rule is only correct inside one
// attempt, which is the store's own identity for an artifact. This pins the
// rule it protects: two attempts each producing the same path are not a
// duplicate, they are two attempts, and they are refused as two grants.
func TestTwoAttemptsSharingAPathAreRefusedAsTwoGrantsNotADuplicate(t *testing.T) {
	first := sampleArtifactManifest()
	second := sampleArtifactManifest()
	second.AttemptID = "018f3a2b-7c41-7d8e-8f90-a1b2c3d4e5f6"
	err := ValidateArtifactSet([]ArtifactManifest{first, second})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("want refusal, got %v", err)
	}
	if !strings.Contains(err.Error(), "more than one grant") {
		t.Fatalf("reported as a duplicate path rather than two grants: %v", err)
	}
}

func TestASingleManifestAlwaysNamesOneGrant(t *testing.T) {
	if err := ValidateArtifactSet([]ArtifactManifest{sampleArtifactManifest()}); err != nil {
		t.Fatalf("one artifact is one grant: %v", err)
	}
	if err := ValidateArtifactSet(nil); err != nil {
		t.Fatalf("an attempt with no artifacts is normal: %v", err)
	}
}

// The check compares whole grants, so it must not start refusing a set whose
// entries differ in everything a grant does not cover.
func TestASetIsStillAcceptedWhenOnlyTheArtifactDiffers(t *testing.T) {
	first := sampleArtifactManifest()
	second := otherGrantManifest()
	second.StepName = "flash"
	second.TotalBytes = first.TotalBytes + 1
	second.FinalSequence = 1
	second.Truncated = true
	second.CapturedAt = first.CapturedAt.Add(1)
	if err := ValidateArtifactSet([]ArtifactManifest{first, second}); err != nil {
		t.Fatalf("entries may differ in everything but their grant: %v", err)
	}
}

// An invalid entry is still reported as invalid rather than as a grant
// disagreement: Validate runs on each entry before it is compared.
func TestAMalformedEntryIsStillJudgedOnItsOwnTerms(t *testing.T) {
	broken := otherGrantManifest()
	broken.TotalBytes = 0
	broken.AttemptID = "018f3a2b-7c41-7d8e-8f90-a1b2c3d4e5f6"
	err := ValidateArtifactSet([]ArtifactManifest{sampleArtifactManifest(), broken})
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("want refusal, got %v", err)
	}
	if strings.Contains(err.Error(), "more than one grant") {
		t.Fatalf("a malformed entry was reported as a grant disagreement: %v", err)
	}
}

// checkArtifactSetNamesOneAttempt is the whole rule, so it answers on the four
// grant fields and on nothing else.
func TestTheGrantRuleReadsTheFourFencingFields(t *testing.T) {
	first := sampleArtifactManifest()
	if err := checkArtifactSetNamesOneAttempt(first, first); err != nil {
		t.Fatalf("a manifest disagrees with itself: %v", err)
	}
	same := first
	same.Path = "other.bin"
	same.StepName = "flash"
	same.SHA256 = strings.Repeat("a", 64)
	if err := checkArtifactSetNamesOneAttempt(first, same); err != nil {
		t.Fatalf("non-grant fields were read: %v", err)
	}
}
