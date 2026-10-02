// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import "testing"

// Withholding decides whether this plane ASKS for a new gate. A task the
// evidence backs and the operator has already required is the case where both
// answers agree, and it must come back as a plain Keep: naming it as withheld
// would report work nobody has to do, and dropping it from Keep would read as
// a proposal to take protection off.
func TestAReadyTaskTheGateAlreadyCarriesIsKeptAndNotWithheld(t *testing.T) {
	context := requiredName(t, "format")
	readiness := ShadowReadiness{Threshold: 3, Ready: []string{"format"}}

	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, []string{context})
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	if !namesContext(plan.Plan.Keep, context) {
		t.Fatalf("Keep %v does not carry %q", plan.Plan.Keep, context)
	}
	if len(plan.Plan.Add) != 0 {
		t.Fatalf("a context already required was proposed again: %v", plan.Plan.Add)
	}
	if _, withheld := withheldFor(plan, "format"); withheld {
		t.Fatal("a ready task already on the gate was reported as withheld")
	}
	if plan.Withholds() {
		t.Fatalf("nothing should be withheld, got %v", plan.Withheld)
	}
	if !plan.Plan.NoChange() {
		t.Fatal("a gate that already matches the evidence asks for no change")
	}
}

// The three answers land in one plan at once: a ready task the gate has, a
// ready task it does not, and a conflicting task it does. Only the last is
// withheld, and it is still kept, because withholding never takes protection
// an operator put there off the gate.
func TestOneGatePlanSeparatesTheReadyFromTheConflictingAcrossKeepAndAdd(t *testing.T) {
	held := requiredName(t, "format")
	conflicting := requiredName(t, "build")
	readiness := ShadowReadiness{
		Threshold:   2,
		Ready:       []string{"format", "unit-tests"},
		Conflicting: []string{"build"},
	}

	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, []string{held, conflicting})
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	if !namesContext(plan.Plan.Keep, held) || !namesContext(plan.Plan.Keep, conflicting) {
		t.Fatalf("Keep %v should carry both contexts the gate already has", plan.Plan.Keep)
	}
	if !namesContext(plan.Plan.Add, requiredName(t, "unit-tests")) {
		t.Fatalf("Add %v does not propose the ready task the gate lacks", plan.Plan.Add)
	}
	if len(plan.Plan.Add) != 1 {
		t.Fatalf("Add %v proposed more than the one ready task", plan.Plan.Add)
	}
	if _, withheld := withheldFor(plan, "format"); withheld {
		t.Fatal("the ready task already on the gate was withheld")
	}
	if _, withheld := withheldFor(plan, "unit-tests"); withheld {
		t.Fatal("the ready task proposed for the gate was withheld")
	}
	entry, withheld := withheldFor(plan, "build")
	if !withheld {
		t.Fatal("the conflicting task was not named as withheld")
	}
	if entry.Reason != WithheldConflicting {
		t.Fatalf("withheld reason %v, want conflicting", entry.Reason)
	}
	if !entry.AlreadyRequired {
		t.Fatal("a withheld task the gate already carries must say so")
	}
	if entry.Context != conflicting {
		t.Fatalf("withheld context %q, want %q", entry.Context, conflicting)
	}
	if len(plan.Withheld) != 1 {
		t.Fatalf("withheld %v, want the conflicting task alone", plan.Withheld)
	}
}
