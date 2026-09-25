package main

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// The plan is the half of a publish document the two deciding functions build
// their answer from, and neither of them asked whether it names a run once.
// These tests hold it to one run per name, and pin what is deliberately left
// to the reconciler.

// plannedAs is a real planned run put under another run's name, the shape a
// caller assembling a plan from two documents produces. It goes through
// plannedRun first so everything else about the run is what the publisher
// would actually post.
func plannedAs(t *testing.T, task, state, name string) plannedCheckRun {
	t.Helper()
	plan := plannedRun(t, github.ModeShadow, task, state)
	plan.Run.Name = name
	return plan
}

// bothCommandsRefusePlan holds the two entry points to one refusal about the
// plan, the way bothCommandsRefuse does for the listing.
func bothCommandsRefusePlan(t *testing.T, plan []plannedCheckRun, want string) {
	t.Helper()
	_, err := reconcileCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), want) {
		t.Fatalf("reconcile error = %v, want it to name %q", err, want)
	}
	_, err = surveyCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), want) {
		t.Fatalf("survey error = %v, want it to name %q", err, want)
	}
}

// One task planned twice posts two check runs under one name from a single
// document: the commit is listed once, before either of them exists, so both
// decide a run is needed.
func TestATaskPlannedTwiceIsRefusedBeforeAnythingIsPosted(t *testing.T) {
	task := firstCatalogTask(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, task, "failed"),
		plannedRun(t, github.ModeShadow, task, "failed"),
	}
	bothCommandsRefusePlan(t, plan, "planned twice")
	if !strings.Contains(refusalOf(t, plan), task) {
		t.Fatalf("refusal = %q, want the task named", refusalOf(t, plan))
	}
}

// Two tasks under one name are the same fault reached the other way, and the
// refusal names both of them: the reader holds a document of task outcomes
// and the name is something this plane derived from one of them.
func TestTwoTasksPlannedUnderOneNameAreRefused(t *testing.T) {
	one, two := twoPlannedTasks(t)
	first := plannedRun(t, github.ModeShadow, one, "failed")
	plan := []plannedCheckRun{first, plannedAs(t, two, "succeeded", first.Run.Name)}

	bothCommandsRefusePlan(t, plan, "both planned as")
	refusal := refusalOf(t, plan)
	for _, want := range []string{one, two, first.Run.Name} {
		if !strings.Contains(refusal, want) {
			t.Fatalf("refusal = %q, want %q named", refusal, want)
		}
	}
}

// The subject is read before the names. A plan carrying one task on two
// commits is a document about two commits, whatever it calls its runs, and
// that is the refusal an operator can act on.
func TestTheSubjectIsReadBeforeTheNames(t *testing.T) {
	task := firstCatalogTask(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, task, "failed"),
		plannedRunOn(t, github.ModeShadow, task, "failed", otherCommit),
	}

	_, err := reconcileCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), otherCommit) {
		t.Fatalf("error = %v, want the second commit named", err)
	}
	if strings.Contains(err.Error(), "planned twice") {
		t.Fatalf("error = %v, want no duplicate-name refusal in it", err)
	}
}

// The plan is read before the listing. Both arguments can be wrong at once,
// and the document in hand is the one its sender can fix.
func TestThePlanIsReadBeforeTheListing(t *testing.T) {
	task := firstCatalogTask(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, task, "failed"),
		plannedRun(t, github.ModeShadow, task, "failed"),
	}

	_, err := reconcileCheckRunPlan(plan, listing(listedRun(0, "ra8ci (shadow): build")))
	if err == nil || !strings.Contains(err.Error(), "planned twice") {
		t.Fatalf("error = %v, want the plan refusal", err)
	}
	if strings.Contains(err.Error(), "no identifier") {
		t.Fatalf("error = %v, want no listing refusal in it", err)
	}
}

// A blank planned name is deliberately not refused here. ReconcilePublish
// refuses an intended run without one, in both paths and before anything is
// posted, and a second definition of that rule in the command is how the two
// drift apart.
func TestABlankPlannedNameIsLeftToTheReconciler(t *testing.T) {
	plan := []plannedCheckRun{plannedAs(t, firstCatalogTask(t), "failed", "")}
	if err := checkPlanNamesEachRunOnce(plan); err != nil {
		t.Fatalf("a blank name was refused by the plan check: %v", err)
	}

	_, err := reconcileCheckRunPlan(plan, listing())
	if !errors.Is(err, github.ErrReconcileRunIncomplete) {
		t.Fatalf("error = %v, want the reconciler's own refusal", err)
	}
}

// Nothing is reported. The survey answers a conflict with a report rather
// than a refusal, so it is worth stating that this one stops it: a report
// counting one task twice is what the check exists to keep out of an
// operator's hands.
func TestADuplicatePlanNeverReachesTheReport(t *testing.T) {
	task := firstCatalogTask(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, task, "failed"),
		plannedRun(t, github.ModeShadow, task, "failed"),
	}

	report, err := surveyCheckRunPlan(plan, listing())
	if err == nil {
		t.Fatal("the survey reported a plan naming one run twice")
	}
	if len(report.Tasks) != 0 || report.Posting != 0 || report.Commit != "" {
		t.Fatalf("report = %+v, want nothing reported", report)
	}
}

// The ordinary document is two real tasks, which carry two names, and neither
// command may refuse it.
func TestAPlanOfDistinctTasksIsNotRefused(t *testing.T) {
	one, two := twoPlannedTasks(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, one, "failed"),
		plannedRun(t, github.ModeShadow, two, "succeeded"),
	}

	if err := checkPlanNamesEachRunOnce(plan); err != nil {
		t.Fatalf("a plan of distinct tasks was refused: %v", err)
	}
	if _, err := reconcileCheckRunPlan(plan, listing()); err != nil {
		t.Fatalf("reconcile refused a plan of distinct tasks: %v", err)
	}
	if _, err := surveyCheckRunPlan(plan, listing()); err != nil {
		t.Fatalf("survey refused a plan of distinct tasks: %v", err)
	}
}

// refusalOf is the message the publish path gives for a plan, so a test can
// read what it names without repeating the call.
func refusalOf(t *testing.T, plan []plannedCheckRun) string {
	t.Helper()
	_, err := reconcileCheckRunPlan(plan, listing())
	if err == nil {
		t.Fatal("the plan was not refused")
	}
	return err.Error()
}
