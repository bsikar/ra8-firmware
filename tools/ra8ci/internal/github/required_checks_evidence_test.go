// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

const (
	evidencePlanCommitA = "1111111111111111111111111111111111111111"
	evidencePlanCommitB = "2222222222222222222222222222222222222222"
	evidencePlanCommitC = "3333333333333333333333333333333333333333"
)

// readinessOver grades these reports and reads the answer at one threshold, so
// the plans under test are built from comparisons CompareShadowRun actually
// produced rather than from readiness lists typed by hand.
func readinessOver(t *testing.T, threshold int, reports ...ShadowReport) ShadowReadiness {
	t.Helper()
	evidence, err := AccumulateShadowEvidence(reports)
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	readiness, err := evidence.Readiness(threshold)
	if err != nil {
		t.Fatalf("Readiness: %v", err)
	}
	return readiness
}

func requiredName(t *testing.T, task string) string {
	t.Helper()
	name, err := CheckRunName(ModeAuthoritative, task)
	if err != nil {
		t.Fatalf("CheckRunName(%q): %v", task, err)
	}
	return name
}

func namesContext(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}

func withheldFor(plan EvidenceBackedPlan, task string) (WithheldTask, bool) {
	for _, withheld := range plan.Withheld {
		if withheld.Task == task {
			return withheld, true
		}
	}
	return WithheldTask{}, false
}

// Only a task the evidence backs may be proposed for the gate. The other two
// are the whole point of #1481's hold: one ra8ci has already been wrong about,
// one nobody has compared often enough to say.
func TestOnlyTasksTheEvidenceBacksAreProposedForTheGate(t *testing.T) {
	readiness := readinessOver(t, 2,
		gradedReport(t, evidencePlanCommitA, agreeing("build"), conflicting("flash"), agreeing("lint")),
		gradedReport(t, evidencePlanCommitB, agreeing("build"), agreeing("flash"), ungraded("lint")),
	)
	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}

	if len(plan.Plan.Add) != 1 || plan.Plan.Add[0] != requiredName(t, "build") {
		t.Fatalf("Add = %v, want only %q", plan.Plan.Add, requiredName(t, "build"))
	}
	if plan.Threshold != 2 {
		t.Fatalf("Threshold = %d, want 2", plan.Threshold)
	}
	if !plan.Withholds() {
		t.Fatal("Withholds() = false, want true")
	}

	flash, ok := withheldFor(plan, "flash")
	if !ok {
		t.Fatal("flash is not withheld")
	}
	if flash.Reason != WithheldConflicting {
		t.Fatalf("flash reason = %v, want conflicting", flash.Reason)
	}
	if flash.Context != requiredName(t, "flash") {
		t.Fatalf("flash context = %q, want %q", flash.Context, requiredName(t, "flash"))
	}
	if flash.AlreadyRequired {
		t.Fatal("flash reads as already required, but the gate was empty")
	}

	lint, ok := withheldFor(plan, "lint")
	if !ok {
		t.Fatal("lint is not withheld")
	}
	if lint.Reason != WithheldInsufficient {
		t.Fatalf("lint reason = %v, want insufficient", lint.Reason)
	}
}

// Every covered task lands in exactly one of Add and Withheld: an unbacked task
// must be named, never dropped, and a backed one must not be reported as held.
func TestEveryCoveredTaskIsEitherProposedOrNamedAsWithheld(t *testing.T) {
	readiness := readinessOver(t, 1,
		gradedReport(t, evidencePlanCommitA, agreeing("build"), conflicting("flash"), ungraded("lint")),
	)
	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	for _, task := range []string{"build", "flash", "lint"} {
		context := requiredName(t, task)
		_, held := withheldFor(plan, task)
		proposed := namesContext(plan.Plan.Add, context)
		if held == proposed {
			t.Fatalf("task %q: proposed=%v withheld=%v, want exactly one", task, proposed, held)
		}
	}
}

// The mode decides whether the gate may move at all, so a shadow deployment
// proposes nothing and the evidence gets no credit for a decision the mode made.
func TestAShadowDeploymentWithholdsNothingBecauseItProposesNothing(t *testing.T) {
	readiness := readinessOver(t, 1,
		gradedReport(t, evidencePlanCommitA, agreeing("build"), conflicting("flash")),
	)
	plan, err := PlanRequiredChecksFromEvidence(ModeShadow, readiness, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	if len(plan.Plan.Add) != 0 {
		t.Fatalf("Add = %v, want none in shadow mode", plan.Plan.Add)
	}
	if plan.Withholds() {
		t.Fatalf("Withheld = %v, want none: the mode refused, not the evidence", plan.Withheld)
	}
	if plan.Plan.Mode != ModeShadow {
		t.Fatalf("Mode = %v, want shadow", plan.Plan.Mode)
	}
}

// Withholding decides whether this plane ASKS for a new gate. Protection an
// operator already put there is a different decision, and not one the evidence
// makes, so a withheld task whose context is required today is kept and said out
// loud rather than taken off.
func TestAGateTheOperatorAlreadyHasIsKeptAndReported(t *testing.T) {
	readiness := readinessOver(t, 3,
		gradedReport(t, evidencePlanCommitA, conflicting("flash")),
	)
	context := requiredName(t, "flash")
	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, []string{context})
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	if !namesContext(plan.Plan.Keep, context) {
		t.Fatalf("Keep = %v, want %q kept", plan.Plan.Keep, context)
	}
	if namesContext(plan.Plan.Remove, context) {
		t.Fatalf("Remove = %v, want the evidence never to take a gate off", plan.Plan.Remove)
	}
	withheld, ok := withheldFor(plan, "flash")
	if !ok {
		t.Fatal("flash is not reported as withheld")
	}
	if !withheld.AlreadyRequired {
		t.Fatal("AlreadyRequired = false, want the report to say the gate runs ahead of the evidence")
	}
}

// The same evidence plans differently at a different threshold, and the answer
// carries the threshold it was read at.
func TestTheThresholdDecidesThePlanAndTravelsWithIt(t *testing.T) {
	reports := []ShadowReport{
		gradedReport(t, evidencePlanCommitA, agreeing("build")),
		gradedReport(t, evidencePlanCommitB, agreeing("build")),
	}
	at2, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readinessOver(t, 2, reports...), nil)
	if err != nil {
		t.Fatalf("at threshold 2: %v", err)
	}
	at3, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readinessOver(t, 3, reports...), nil)
	if err != nil {
		t.Fatalf("at threshold 3: %v", err)
	}
	if len(at2.Plan.Add) != 1 || at2.Threshold != 2 {
		t.Fatalf("at 2: Add = %v, threshold = %d, want one addition at 2", at2.Plan.Add, at2.Threshold)
	}
	if len(at3.Plan.Add) != 0 || at3.Threshold != 3 {
		t.Fatalf("at 3: Add = %v, threshold = %d, want nothing added at 3", at3.Plan.Add, at3.Threshold)
	}
	if held, ok := withheldFor(at3, "build"); !ok || held.Reason != WithheldInsufficient {
		t.Fatalf("at 3: build withheld = %v/%v, want insufficient", held, ok)
	}
}

// A shadow name can only ever report neutral, so it comes off the gate whatever
// the evidence says about the task behind it, and it is never reported as
// withheld: nothing proposed to add it.
func TestAShadowContextComesOffTheGateWhateverTheEvidenceSays(t *testing.T) {
	readiness := readinessOver(t, 1,
		gradedReport(t, evidencePlanCommitA, agreeing("build")),
	)
	shadowContext, err := CheckRunName(ModeShadow, "build")
	if err != nil {
		t.Fatalf("CheckRunName: %v", err)
	}
	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, []string{shadowContext})
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	if !namesContext(plan.Plan.Remove, shadowContext) {
		t.Fatalf("Remove = %v, want %q removed", plan.Plan.Remove, shadowContext)
	}
	if plan.Withholds() {
		t.Fatalf("Withheld = %v, want none", plan.Withheld)
	}
}

// A context this plane does not own is reported and left alone, whether or not
// the evidence backs anything.
func TestForeignContextsAreNeitherWithheldNorTouched(t *testing.T) {
	readiness := readinessOver(t, 2,
		gradedReport(t, evidencePlanCommitA, conflicting("flash")),
	)
	foreign := []string{"CodeQL", "build (ubuntu-latest)", "ra8ci-lint / build"}
	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, foreign)
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	if len(plan.Plan.Foreign) != len(foreign) {
		t.Fatalf("Foreign = %v, want all %v", plan.Plan.Foreign, foreign)
	}
	if len(plan.Plan.Remove) != 0 {
		t.Fatalf("Remove = %v, want nothing a foreign name owns", plan.Plan.Remove)
	}
	for _, withheld := range plan.Withheld {
		if namesContext(foreign, withheld.Context) {
			t.Fatalf("withheld a foreign context %q", withheld.Context)
		}
	}
}

func TestEvidenceBackedPlanRefusesWhatItCannotPlanHonestly(t *testing.T) {
	good := readinessOver(t, 1, gradedReport(t, evidencePlanCommitA, agreeing("build")))

	cases := []struct {
		name      string
		mode      CheckRunMode
		readiness ShadowReadiness
		required  []string
		want      error
	}{
		{
			name:      "unknown mode",
			mode:      CheckRunMode(7),
			readiness: good,
			want:      ErrInvalidCheckRunMode,
		},
		{
			name:      "readiness nobody asked for",
			mode:      ModeAuthoritative,
			readiness: ShadowReadiness{Ready: []string{"build"}},
			want:      ErrShadowEvidenceThresholdInvalid,
		},
		{
			name:      "evidence covering nothing",
			mode:      ModeAuthoritative,
			readiness: ShadowReadiness{Threshold: 2},
			want:      ErrRequiredCheckEvidenceEmpty,
		},
		{
			name:      "one task in two lists",
			mode:      ModeAuthoritative,
			readiness: ShadowReadiness{Threshold: 1, Ready: []string{"build"}, Conflicting: []string{"build"}},
			want:      ErrRequiredCheckSetAmbiguous,
		},
		{
			name:      "task name no catalog carries",
			mode:      ModeAuthoritative,
			readiness: ShadowReadiness{Threshold: 1, Ready: []string{"not a task"}},
			want:      ErrRequiredCheckTaskInvalid,
		},
		{
			name:      "padded required context",
			mode:      ModeAuthoritative,
			readiness: good,
			required:  []string{" CodeQL"},
			want:      ErrRequiredCheckContextInvalid,
		},
		{
			name:      "same context twice",
			mode:      ModeAuthoritative,
			readiness: good,
			required:  []string{"CodeQL", "CodeQL"},
			want:      ErrRequiredCheckSetAmbiguous,
		},
	}

	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			plan, err := PlanRequiredChecksFromEvidence(testCase.mode, testCase.readiness, testCase.required)
			if !errors.Is(err, testCase.want) {
				t.Fatalf("error = %v, want %v", err, testCase.want)
			}
			if len(plan.Plan.Add) != 0 || len(plan.Plan.Remove) != 0 || len(plan.Plan.Keep) != 0 ||
				len(plan.Plan.Foreign) != 0 || len(plan.Withheld) != 0 || plan.Threshold != 0 {
				t.Fatalf("refused plan carries %+v, want the zero value", plan)
			}
		})
	}
}

// The reasons are named in the report an operator reads, so a withheld task says
// which of the two kinds of work it is waiting on.
func TestWithheldReasonsAreNamed(t *testing.T) {
	cases := map[WithheldReason]string{
		WithheldInsufficient: "insufficient",
		WithheldConflicting:  "conflicting",
		WithheldReason(99):   "unknown",
	}
	for reason, want := range cases {
		if got := reason.String(); got != want {
			t.Fatalf("WithheldReason(%d).String() = %q, want %q", int(reason), got, want)
		}
	}
	if strings.TrimSpace(WithheldConflicting.String()) != WithheldConflicting.String() {
		t.Fatal("reason names must not carry surrounding whitespace")
	}
}

// The evidence never invents a task. A catalog task no pull request exercised is
// not in the readiness answer, so it is neither added nor reported as withheld,
// and `ra8ci github required-checks` still reports it as uncovered.
func TestATaskTheEvidenceNeverSawIsNotInThePlanAtAll(t *testing.T) {
	readiness := readinessOver(t, 1,
		gradedReport(t, evidencePlanCommitA, agreeing("build")),
	)
	plan, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}
	unseen := requiredName(t, "flash")
	if namesContext(plan.Plan.Add, unseen) || namesContext(plan.Plan.Keep, unseen) {
		t.Fatalf("plan names %q, which no report mentioned", unseen)
	}
	if _, held := withheldFor(plan, "flash"); held {
		t.Fatal("flash is withheld, but no report ever mentioned it")
	}
}

// The evidence takes names out of Add and changes nothing else about the plan
// required_checks.go computed for the same tasks.
func TestEvidenceOnlyNarrowsTheAdditions(t *testing.T) {
	readiness := readinessOver(t, 2,
		gradedReport(t, evidencePlanCommitA, agreeing("build"), ungraded("lint")),
		gradedReport(t, evidencePlanCommitB, agreeing("build")),
		gradedReport(t, evidencePlanCommitC, agreeing("build")),
	)
	stale := requiredName(t, "removed-task")
	required := []string{stale, "CodeQL"}

	plain, err := PlanRequiredChecks(ModeAuthoritative, []string{"build", "lint"}, required)
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	backed, err := PlanRequiredChecksFromEvidence(ModeAuthoritative, readiness, required)
	if err != nil {
		t.Fatalf("PlanRequiredChecksFromEvidence: %v", err)
	}

	if len(plain.Add) != 2 {
		t.Fatalf("plain Add = %v, want both tasks proposed", plain.Add)
	}
	if len(backed.Plan.Add) != 1 || backed.Plan.Add[0] != requiredName(t, "build") {
		t.Fatalf("backed Add = %v, want only build", backed.Plan.Add)
	}
	if !namesContext(backed.Plan.Remove, stale) || len(backed.Plan.Remove) != len(plain.Remove) {
		t.Fatalf("Remove = %v, want the same removals as %v", backed.Plan.Remove, plain.Remove)
	}
	if len(backed.Plan.Foreign) != len(plain.Foreign) || !namesContext(backed.Plan.Foreign, "CodeQL") {
		t.Fatalf("Foreign = %v, want the same as %v", backed.Plan.Foreign, plain.Foreign)
	}
	if len(backed.Plan.Keep) != len(plain.Keep) {
		t.Fatalf("Keep = %v, want the same as %v", backed.Plan.Keep, plain.Keep)
	}
}
