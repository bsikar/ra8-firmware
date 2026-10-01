package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// The two functions that decide a whole publish document read its subject off
// the first planned run: the publish path lists that commit's check runs once
// and reconciles everything against the listing, and the survey states that
// commit and that mode at the top of the report. These tests hold the plan to
// that subject, and pin what is deliberately left alone.

// otherCommit is a second head, used to build a plan about two commits. It is
// a valid SHA and is not shadowCompareHead.
const otherCommit = "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c"

// plannedRunOn is plannedRun for a commit other than shadowCompareHead. It
// goes through NewTaskCheckRun for the same reason plannedRun does: the
// refusals are about runs the publisher would actually post.
func plannedRunOn(t *testing.T, mode github.CheckRunMode, task, state, headSHA string) plannedCheckRun {
	t.Helper()
	run, err := github.NewTaskCheckRun(mode, task, headSHA, state)
	if err != nil {
		t.Fatalf("build the intended run: %v", err)
	}
	return plannedCheckRun{Task: task, Run: run, Summary: "ra8ci observed " + state}
}

// twoPlannedTasks is twoCatalogTasks, named for what these tests need it for:
// two real reviewed task names, so a plan can carry two runs without tripping
// the duplicate-task refusal in planCheckRuns.
func twoPlannedTasks(t *testing.T) (string, string) {
	t.Helper()
	return twoCatalogTasks(t)
}

func TestAPlanAboutTwoCommitsIsRefusedBeforeAnythingIsDecided(t *testing.T) {
	one, two := twoPlannedTasks(t)
	first := plannedRun(t, github.ModeShadow, one, "failed")
	elsewhere := plannedRunOn(t, github.ModeShadow, two, "succeeded", otherCommit)
	plan := []plannedCheckRun{first, elsewhere}

	_, err := reconcileCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), otherCommit) ||
		!strings.Contains(err.Error(), shadowCompareHead) {
		t.Fatalf("reconcile error = %v, want both commits named", err)
	}
	_, err = surveyCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), otherCommit) {
		t.Fatalf("survey error = %v, want the second commit named", err)
	}
}

func TestAPlanAboutTwoModesIsRefused(t *testing.T) {
	one, two := twoPlannedTasks(t)
	shadow := plannedRun(t, github.ModeShadow, one, "failed")
	authoritative := plannedRun(t, github.ModeAuthoritative, two, "succeeded")
	plan := []plannedCheckRun{shadow, authoritative}

	_, err := reconcileCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), github.ModeAuthoritative.String()) ||
		!strings.Contains(err.Error(), github.ModeShadow.String()) {
		t.Fatalf("reconcile error = %v, want both modes named", err)
	}
	_, err = surveyCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), github.ModeShadow.String()) {
		t.Fatalf("survey error = %v, want the document's mode named", err)
	}
}

// The refusal names the task, not the run name. A reader of either command
// holds a document of task outcomes, and the check run name is something this
// plane derived from one of them.
func TestTheRefusalNamesTheTaskThatDisagrees(t *testing.T) {
	one, odd := twoPlannedTasks(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, one, "failed"),
		plannedRunOn(t, github.ModeShadow, odd, "succeeded", otherCommit),
	}
	_, err := reconcileCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), odd) {
		t.Fatalf("error = %v, want the task %q named", err, odd)
	}
}

// The commit is read before the mode. A run that disagrees about both is
// refused for the commit: the listing is read for one commit and a document
// spanning two is answered by no listing at all, while a mode is a question
// about what the one listing was compared against.
func TestTheCommitIsReadBeforeTheMode(t *testing.T) {
	one, two := twoPlannedTasks(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, one, "failed"),
		plannedRunOn(t, github.ModeAuthoritative, two, "succeeded", otherCommit),
	}
	_, err := reconcileCheckRunPlan(plan, listing())
	if err == nil || !strings.Contains(err.Error(), otherCommit) {
		t.Fatalf("error = %v, want the commit refusal", err)
	}
	if strings.Contains(err.Error(), "mode") {
		t.Fatalf("error = %v, want no mode in the commit refusal", err)
	}
}

// A commit GitHub rendered in the other case is the same commit. The listing
// reader lowers what it reports and ReconcilePublish compares case-insensitively,
// so refusing here would refuse a plan both of those accept.
func TestACommitInAnotherCaseIsTheSameSubject(t *testing.T) {
	one, two := twoPlannedTasks(t)
	first := plannedRun(t, github.ModeShadow, one, "failed")
	same := plannedRun(t, github.ModeShadow, two, "succeeded")
	same.Run.HeadSHA = strings.ToUpper(same.Run.HeadSHA)
	if err := checkPlanIsOneSubject([]plannedCheckRun{first, same}); err != nil {
		t.Fatalf("the same commit in upper case was refused: %v", err)
	}
}

// An ordinary document is many tasks on one commit in one mode, and it is what
// planCheckRuns builds every time. Nothing about this check narrows that.
func TestAnOrdinaryPlanOfManyTasksIsNotRefused(t *testing.T) {
	one, two := twoPlannedTasks(t)
	plan := []plannedCheckRun{
		plannedRun(t, github.ModeShadow, one, "failed"),
		plannedRun(t, github.ModeShadow, two, "succeeded"),
	}
	report, err := surveyCheckRunPlan(plan, listing())
	if err != nil {
		t.Fatalf("survey a plain plan: %v", err)
	}
	if report.Commit != shadowCompareHead || report.Mode != github.ModeShadow.String() {
		t.Fatalf("report subject = %s %s", report.Commit, report.Mode)
	}
	if report.Posting != 2 {
		t.Fatalf("posting = %d, want 2", report.Posting)
	}
}

// What planCheckRuns builds always passes, which is the point: the check is a
// contract of the two deciding functions, not a new rule on the documents this
// command accepts.
func TestAPlanBuiltByThisCommandIsAlwaysOneSubject(t *testing.T) {
	first, second := twoPlannedTasks(t)
	correspondence, err := github.NewShadowCorrespondence(
		map[string]string{first: "build", second: "test"}, []string{first, second})
	if err != nil {
		t.Fatalf("correspondence: %v", err)
	}
	document := planCheckRunsInput(first, "failed", "")
	document.Runs = append(document.Runs, struct {
		Task    string `json:"task"`
		State   string `json:"state"`
		Summary string `json:"summary"`
	}{Task: second, State: "succeeded", Summary: ""})

	planned, err := planCheckRuns(github.ModeShadow, correspondence, document)
	if err != nil {
		t.Fatalf("plan the runs: %v", err)
	}
	if err := checkPlanIsOneSubject(planned); err != nil {
		t.Fatalf("a planned document was refused: %v", err)
	}
}
