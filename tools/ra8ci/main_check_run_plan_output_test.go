package main

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// The publisher holds every output body to GitHub's ceilings as it posts, and
// that is the last place the rule can be applied: a document whose later
// summary is too long is refused with its earlier runs already on the commit,
// and a check run cannot be taken back. These tests hold the plan to the same
// rule, where nothing has been posted yet, and pin what is deliberately left
// to the caller.

// summaryPlanner is the correspondence and the tasks a planning test needs:
// two real reviewed task names, both covered, so a document can carry two
// runs without tripping the duplicate-task or uncovered-task refusals.
func summaryPlanner(t *testing.T) (*github.ShadowCorrespondence, string, string) {
	t.Helper()
	one, two := twoCatalogTasks(t)
	correspondence, err := github.NewShadowCorrespondence(
		map[string]string{one: "build", two: "test"}, []string{one, two})
	if err != nil {
		t.Fatalf("correspondence: %v", err)
	}
	return correspondence, one, two
}

// twoTaskDocument is planCheckRunsInput for two tasks on one commit, each with
// the summary it was given.
func twoTaskDocument(first, firstSummary, second, secondSummary string) checkRunPublishInput {
	document := planCheckRunsInput(first, "succeeded", firstSummary)
	document.Runs = append(document.Runs, planCheckRunsInput(second, "succeeded", secondSummary).Runs...)
	return document
}

// overLongSummary is past GitHub's ceiling for a check run summary by any
// reading of it: more characters than the API accepts, written in one-byte
// characters so the count is not a question of encoding.
func overLongSummary() string { return strings.Repeat("a", 70000) }

func TestASummaryGitHubWouldRefuseIsRefusedBeforeAnythingIsPosted(t *testing.T) {
	correspondence, task, _ := summaryPlanner(t)

	_, err := planCheckRuns(github.ModeShadow, correspondence,
		planCheckRunsInput(task, "succeeded", overLongSummary()))
	if err == nil {
		t.Fatal("a summary past GitHub's ceiling was planned")
	}
	if !errors.Is(err, github.ErrCheckRunOutputUnusable) {
		t.Fatalf("refusal %v, want the publisher's own output rule", err)
	}
	if !strings.Contains(err.Error(), "70000") {
		t.Fatalf("refusal %q does not say how long the summary was", err)
	}
}

// The refusal names the task. A reader of either command holds a document of
// task outcomes; the check run name and the title are things this plane
// derived from one of them.
func TestTheRefusalNamesTheTaskWhoseSummaryCannotBePublished(t *testing.T) {
	correspondence, task, _ := summaryPlanner(t)

	_, err := planCheckRuns(github.ModeShadow, correspondence,
		planCheckRunsInput(task, "succeeded", overLongSummary()))
	if err == nil || !strings.Contains(err.Error(), task) {
		t.Fatalf("refusal %v, want the task named", err)
	}
}

// The point of reading the rule here rather than leaving it to the publisher:
// the good run earlier in the document is not posted first and then orphaned
// by the refusal that follows it.
func TestADocumentIsRefusedWholeWhenALaterSummaryIsTooLong(t *testing.T) {
	correspondence, first, second := summaryPlanner(t)

	planned, err := planCheckRuns(github.ModeShadow, correspondence,
		twoTaskDocument(first, "ran on the bench", second, overLongSummary()))
	if err == nil {
		t.Fatal("a document with an unpublishable run in it was planned")
	}
	if !strings.Contains(err.Error(), second) || strings.Contains(err.Error(), first) {
		t.Fatalf("refusal %v, want only the unpublishable task named", err)
	}
	if planned != nil {
		t.Fatalf("planned %d runs alongside the refusal, want none", len(planned))
	}
}

// Nothing ordinary is refused: the summary a caller writes and the one this
// plane composes are both published as they stand.
func TestAnOrdinaryDocumentIsStillPlannedWhole(t *testing.T) {
	correspondence, first, second := summaryPlanner(t)

	planned, err := planCheckRuns(github.ModeShadow, correspondence,
		twoTaskDocument(first, "ran on the bench", second, ""))
	if err != nil {
		t.Fatalf("plan: %v", err)
	}
	if len(planned) != 2 {
		t.Fatalf("planned %d runs, want 2", len(planned))
	}
	if planned[0].Summary != "ran on the bench" {
		t.Fatalf("summary %q, want the one the caller wrote", planned[0].Summary)
	}
	if !strings.Contains(planned[1].Summary, second) {
		t.Fatalf("composed summary %q does not name the task", planned[1].Summary)
	}
}

// A blank summary is filled before the rule is read, so the publisher's
// blank-summary refusal is unreachable from here. The composed summary is the
// answer to a blank one, and it is a short restatement of facts the run
// already carries, so it can never be the over-long half either.
func TestABlankSummaryIsFilledRatherThanRefused(t *testing.T) {
	correspondence, task, _ := summaryPlanner(t)

	planned, err := planCheckRuns(github.ModeShadow, correspondence,
		planCheckRunsInput(task, "succeeded", "   "))
	if err != nil {
		t.Fatalf("a blank summary was refused rather than composed: %v", err)
	}
	if strings.TrimSpace(planned[0].Summary) == "" {
		t.Fatal("planned a run with a blank summary")
	}
	if err := github.CheckPublishableOutput(planned[0].Run.Title, planned[0].Summary); err != nil {
		t.Fatalf("the composed summary is not publishable: %v", err)
	}
}

// The ceilings are counted in characters, the unit GitHub states them in. A
// summary written in a script whose characters take more than one byte is
// legal at a length a byte count would refuse, and refusing it here would
// refuse a document the publisher would have posted.
func TestALongMultiByteSummaryIsPlanned(t *testing.T) {
	correspondence, task, _ := summaryPlanner(t)
	summary := strings.Repeat("\u4e16", 60000)

	planned, err := planCheckRuns(github.ModeShadow, correspondence,
		planCheckRunsInput(task, "succeeded", summary))
	if err != nil {
		t.Fatalf("a legal summary of %d bytes was refused: %v", len(summary), err)
	}
	if planned[0].Summary != summary {
		t.Fatal("the planned summary is not the one the caller wrote")
	}
}

// *** REFUSED, NEVER CUT, AND THE PLAN IS NOT THE PLACE TO CHANGE THAT. The
// summary is the only place a check run explains itself, so a published body
// that stops mid-account reads as the whole account. The caller that
// assembled the document is the one that knows what to drop, and the
// publisher's own comment says so. This pins the plan against "helpfully"
// truncating on the caller's behalf. ***
func TestAnUnpublishableSummaryIsRefusedRatherThanCut(t *testing.T) {
	correspondence, task, _ := summaryPlanner(t)

	planned, err := planCheckRuns(github.ModeShadow, correspondence,
		planCheckRunsInput(task, "succeeded", overLongSummary()))
	if err == nil {
		t.Fatal("an over-long summary was planned, presumably cut to fit")
	}
	if len(planned) != 0 {
		t.Fatalf("planned %d runs from a refused document", len(planned))
	}
}
