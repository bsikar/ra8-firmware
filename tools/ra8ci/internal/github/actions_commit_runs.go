// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"path"
)

// PullRequestHeadReader (#1589) says which commit a pull request is at, and
// Outcomes (#1578) grades one workflow run whose ID somebody already knows.
// Between the two there was nothing: an operator gathering #1481's evidence
// over representative pull requests had a head SHA in one hand and no way to
// find the runs GitHub executed on it, so run IDs came out of the web
// interface and into a document by hand.
//
// RunsOn closes that gap by listing the workflow runs GitHub recorded against
// one commit. It is a method on the Actions reader deliberately: listing runs
// for a commit is actions:read, the permission this reader already holds, and
// the seam #1589 drew was that the pull-request token stays pull_requests:read
// and the Actions token stays actions:read rather than one token growing two
// permissions to save a hop.
//
// It lists and reports. Which of a commit's runs is the evidence is not a
// question this file answers: a commit carries release workflows, docs
// workflows and the checks run alike, and picking among them is the operator's
// judgement, the same division CheckRunReconciler draws when it reports a
// commit's check runs without deciding what to do about them.

// maxCommitRunPages bounds the paging, matching the job listing's ceiling of
// ten pages of a hundred.
const maxCommitRunPages = 10

// ErrCommitRunsUnreadable is returned when GitHub did not answer with this
// commit's workflow runs. It is NOT read as a commit with no runs: a commit
// that does not exist, one in another repository and an installation that
// cannot see Actions all answer much the same way, and an empty listing reads
// as a commit CI never touched, which is precisely the state an operator
// choosing representative pull requests must be able to tell apart from a read
// that did not happen.
var ErrCommitRunsUnreadable = errors.New("GitHub did not return this commit's workflow runs")

// CommitWorkflowRun is one workflow run GitHub recorded against a commit,
// reported verbatim.
//
// Attempt is the run's current attempt, which is the one Outcomes will read.
// Status and Conclusion are carried as GitHub wrote them, empty conclusion
// included, so a run still executing is visible as itself rather than as an
// absence.
type CommitWorkflowRun struct {
	ID         int64
	Workflow   string
	Attempt    int
	Event      string
	Status     string
	Conclusion string
}

// Completed reports whether Outcomes will accept this run. A run that has not
// completed is refused there (ErrActionsRunIncomplete), and a caller that can
// ask before the call spends no token on a read it already knows will fail.
func (r CommitWorkflowRun) Completed() bool { return r.Status == "completed" }

// CommitWorkflowRuns is one commit's workflow runs.
type CommitWorkflowRuns struct {
	HeadSHA string
	// Runs are in the order GitHub listed them, newest first.
	Runs []CommitWorkflowRun
}

// Completed returns only the runs Outcomes will grade, in listing order.
func (c CommitWorkflowRuns) Completed() []CommitWorkflowRun {
	completed := []CommitWorkflowRun{}
	for _, run := range c.Runs {
		if run.Completed() {
			completed = append(completed, run)
		}
	}
	return completed
}

type commitRunsResponse struct {
	TotalCount int `json:"total_count"`
	Runs       []struct {
		ID         int64  `json:"id"`
		Name       string `json:"name"`
		RunAttempt int    `json:"run_attempt"`
		HeadSHA    string `json:"head_sha"`
		Event      string `json:"event"`
		Status     string `json:"status"`
		Conclusion string `json:"conclusion"`
		Repository struct {
			FullName string `json:"full_name"`
		} `json:"repository"`
	} `json:"workflow_runs"`
}

// RunsOn lists the workflow runs GitHub recorded against one commit.
//
// Runs still executing are reported, not refused. Outcomes refuses them
// because grading an unfinished run banks pairings a later read would have
// decided; listing is the opposite act, and hiding a queued run from the
// person choosing which pull requests to gather evidence from would tell them
// CI is finished with a commit it has not started on.
//
// A run listed against another commit or another repository refuses the whole
// read, for the reason a job listed under another run does: it is evidence
// about a commit this listing does not report on.
func (r *ActionsOutcomeReader) RunsOn(ctx context.Context, headSHA string) (CommitWorkflowRuns, error) {
	if r == nil || ctx == nil {
		return CommitWorkflowRuns{}, errors.New("invalid commit workflow run request")
	}
	if !validCommitSHA(headSHA) {
		return CommitWorkflowRuns{}, fmt.Errorf("%w: %q is not a commit", ErrInvalidCheckRunSHA, headSHA)
	}
	token, err := r.tokens.accessToken(ctx)
	if err != nil {
		return CommitWorkflowRuns{}, err
	}
	runs := []CommitWorkflowRun{}
	for page := 1; page <= maxCommitRunPages; page++ {
		listed, total, err := r.readCommitRuns(ctx, token, headSHA, page)
		if err != nil {
			return CommitWorkflowRuns{}, err
		}
		runs = append(runs, listed...)
		if len(listed) == 0 || page*100 >= total {
			return CommitWorkflowRuns{HeadSHA: headSHA, Runs: runs}, nil
		}
	}
	return CommitWorkflowRuns{}, fmt.Errorf("%w: commit %s carries more runs than this reader lists", ErrCommitRunsUnreadable, headSHA)
}

// readCommitRuns reads one page of the commit's runs and reports the total
// GitHub says it holds.
func (r *ActionsOutcomeReader) readCommitRuns(ctx context.Context, token, headSHA string, page int) ([]CommitWorkflowRun, int, error) {
	endpoint := *r.apiURL
	endpoint.Path = path.Join(endpoint.Path, "repos", r.config.Owner, r.config.Repository, "actions", "runs")
	query := url.Values{}
	query.Set("head_sha", headSHA)
	query.Set("per_page", "100")
	query.Set("page", fmt.Sprint(page))
	endpoint.RawQuery = query.Encode()
	body, err := r.get(ctx, token, endpoint, "commit workflow runs")
	if err != nil {
		return nil, 0, fmt.Errorf("%w: %s", ErrCommitRunsUnreadable, err)
	}
	var listing commitRunsResponse
	if err := json.Unmarshal(body, &listing); err != nil || listing.TotalCount < 0 || len(listing.Runs) > 100 {
		return nil, 0, fmt.Errorf("%w: unreadable runs document", ErrCommitRunsUnreadable)
	}
	runs := make([]CommitWorkflowRun, 0, len(listing.Runs))
	for _, run := range listing.Runs {
		if run.ID <= 0 || run.RunAttempt < 1 {
			return nil, 0, fmt.Errorf("%w: a listed run does not describe itself", ErrCommitRunsUnreadable)
		}
		if !equalCommit(run.HeadSHA, headSHA) {
			return nil, 0, fmt.Errorf("%w: run %d is on commit %s", ErrCommitRunsUnreadable, run.ID, run.HeadSHA)
		}
		if run.Repository.FullName != "" && run.Repository.FullName != r.config.Owner+"/"+r.config.Repository {
			return nil, 0, fmt.Errorf("%w: run %d belongs to %s", ErrCommitRunsUnreadable, run.ID, run.Repository.FullName)
		}
		runs = append(runs, CommitWorkflowRun{
			ID: run.ID, Workflow: run.Name, Attempt: run.RunAttempt,
			Event: run.Event, Status: run.Status, Conclusion: run.Conclusion,
		})
	}
	return runs, listing.TotalCount, nil
}
