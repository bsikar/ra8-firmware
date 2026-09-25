// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

func intendedFor(t *testing.T, mode CheckRunMode, task, head, state string) TaskCheckRun {
	t.Helper()
	run, err := NewTaskCheckRun(mode, task, head, state)
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	return run
}

// The identifier has to be recomputable from the run in hand. A write whose
// answer never arrived left nothing behind to remember, so an identifier that
// could not be derived twice would be useless in exactly the case the
// reconciliation exists for.
func TestTheExternalIDIsDerivedFromTheRunAndNothingElse(t *testing.T) {
	run := intendedFor(t, ModeShadow, "build", testHeadSHA, "succeeded")
	first, err := CheckRunExternalID(run)
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	again, err := CheckRunExternalID(intendedFor(t, ModeShadow, "build", testHeadSHA, "succeeded"))
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	if first != again {
		t.Fatalf("the same run computed %q and %q", first, again)
	}
	upper, err := CheckRunExternalID(intendedFor(t, ModeShadow, "build", strings.ToUpper(testHeadSHA), "succeeded"))
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	if upper != first {
		t.Fatalf("GitHub's casing of the commit changed the identifier: %q, %q", upper, first)
	}
}

// A run republished after a different observation is the same run. The
// reconciliation has to recognise it in order to report the disagreement
// rather than post a second run under the same name, so the conclusion is
// deliberately not part of the identifier.
func TestTheExternalIDDoesNotDependOnWhatWasObserved(t *testing.T) {
	failed, err := CheckRunExternalID(intendedFor(t, ModeAuthoritative, "build", testHeadSHA, "failed"))
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	succeeded, err := CheckRunExternalID(intendedFor(t, ModeAuthoritative, "build", testHeadSHA, "succeeded"))
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	if failed != succeeded {
		t.Fatalf("the observation changed the identifier: %q, %q", failed, succeeded)
	}
}

func TestTheExternalIDSeparatesTaskModeAndCommit(t *testing.T) {
	other := "89abcdef0123456789abcdef0123456789abcdef"
	base := intendedFor(t, ModeShadow, "build", testHeadSHA, "succeeded")
	cases := map[string]TaskCheckRun{
		"another task":   intendedFor(t, ModeShadow, "unit-tests", testHeadSHA, "succeeded"),
		"another mode":   intendedFor(t, ModeAuthoritative, "build", testHeadSHA, "succeeded"),
		"another commit": intendedFor(t, ModeShadow, "build", other, "succeeded"),
	}
	identifier, err := CheckRunExternalID(base)
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	for name, run := range cases {
		t.Run(name, func(t *testing.T) {
			got, err := CheckRunExternalID(run)
			if err != nil {
				t.Fatalf("external id: %v", err)
			}
			if got == identifier {
				t.Fatalf("%s computed the same identifier %q", name, got)
			}
		})
	}
}

func TestTheExternalIDIsVersionedAndFixedWidth(t *testing.T) {
	identifier, err := CheckRunExternalID(intendedFor(t, ModeShadow, "build", testHeadSHA, "succeeded"))
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	if !strings.HasPrefix(identifier, externalIDVersion+"-") {
		t.Fatalf("identifier %q does not name the derivation it came from", identifier)
	}
	digits := strings.TrimPrefix(identifier, externalIDVersion+"-")
	if len(digits) != externalIDDigits {
		t.Fatalf("identifier %q carries %d digits, want %d", identifier, len(digits), externalIDDigits)
	}
	for _, char := range digits {
		if !strings.ContainsRune("0123456789abcdef", char) {
			t.Fatalf("identifier %q is not lowercase hex", identifier)
		}
	}
}

// A run this package would refuse to publish has no identifier either, so a
// caller cannot post something unpublishable under a well-formed name.
func TestAnUnpublishableRunHasNoExternalID(t *testing.T) {
	for name, run := range map[string]TaskCheckRun{
		"no name":      {HeadSHA: testHeadSHA},
		"no commit":    {Name: "ra8ci-shadow / build"},
		"short commit": {Name: "ra8ci-shadow / build", HeadSHA: "0123456"},
		"zero value":   {},
	} {
		t.Run(name, func(t *testing.T) {
			identifier, err := CheckRunExternalID(run)
			if err == nil || identifier != "" {
				t.Fatalf("built %q for %s", identifier, name)
			}
		})
	}
}

// A published run carrying no identifier is not a match. It is either a run
// posted before this plane wrote the field or a run posted by something else
// under a name of ours, and both are states an operator has to see rather than
// have answered for them.
func TestAPublishedRunWithoutAnIdentifierIsNotAMatch(t *testing.T) {
	run := intendedFor(t, ModeShadow, "build", testHeadSHA, "succeeded")
	identifier, err := CheckRunExternalID(run)
	if err != nil {
		t.Fatalf("external id: %v", err)
	}
	if SameCheckRunExternalID(run, PublishedCheckRun{ID: 1, Name: run.Name, Mode: run.Mode}) {
		t.Fatal("a run with no identifier matched")
	}
	if SameCheckRunExternalID(run, PublishedCheckRun{ID: 1, Name: run.Name, Mode: run.Mode, ExternalID: "ra8ci-1-0000"}) {
		t.Fatal("another deployment's identifier matched")
	}
	if !SameCheckRunExternalID(run, PublishedCheckRun{ID: 1, Name: run.Name, Mode: run.Mode, ExternalID: identifier}) {
		t.Fatalf("the run's own identifier %q did not match", identifier)
	}
	if SameCheckRunExternalID(TaskCheckRun{}, PublishedCheckRun{ExternalID: identifier}) {
		t.Fatal("an unpublishable intended run matched")
	}
}
