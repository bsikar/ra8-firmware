// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path"
)

// Collect (shadow_correspondence.go) grades what this plane observed against
// what the workflow concluded, and until now the Actions half of that pairing
// was a list somebody typed into a document. `ra8ci github shadow-compare`
// reads both sides from stdin for exactly that reason: the correspondence is a
// claim an operator makes, and the command that grades it should not also be
// the one that gathers it.
//
// The gathering still has to happen somewhere. This file reads one completed
// workflow run's job conclusions, so the evidence #1481 holds the required-check
// move against comes off the run GitHub actually executed rather than out of a
// person's editor.
//
// It only reads, and it reads one run. Nothing here pairs a job with a task,
// grades anything, or decides what a usable job name is: those belong to the
// correspondence and the comparison, which already refuse what they cannot
// use. Reporting the run verbatim and letting one place judge it is the same
// division RequiredCheckReader draws with the gate.

const (
	// maxActionsJobPages bounds the paging, matching MetadataResolver's own
	// ceiling of ten pages of a hundred.
	maxActionsJobPages = 10
	// maxActionsRunResponse bounds each response body.
	maxActionsRunResponse = 1 << 20
)

var (
	// ErrActionsRunUnreadable is returned when GitHub did not answer with
	// this run or its jobs. It is NOT read as a run with no jobs: a run
	// that does not exist, one in another repository and an installation
	// that cannot see Actions all answer much the same way, and an empty
	// job list collects as every covered task indeterminate, which is a
	// report saying the comparison was never made rather than one saying
	// the read failed.
	ErrActionsRunUnreadable = errors.New("GitHub did not return this workflow run")
	// ErrActionsRunIncomplete is returned for a run that has not finished.
	// A run still executing has jobs whose conclusions are simply not in
	// yet, and grading it would bank indeterminate pairings as evidence
	// that a later read would have decided.
	ErrActionsRunIncomplete = errors.New("workflow run has not completed")
	// ErrActionsRunTooLarge is returned when a run carries more jobs than
	// this reader will page through.
	ErrActionsRunTooLarge = errors.New("workflow run carries more jobs than this reader collects")
)

// ActionsOutcomeReaderConfig grants actions:read on one repository, which is
// what reading a workflow run's jobs takes.
//
// It carries no mode. Reading what the workflow concluded is the same act
// whether the deployment publishes shadow or authoritative check runs.
type ActionsOutcomeReaderConfig struct {
	APIBaseURL     string
	AppClientID    string
	InstallationID int64
	PrivateKeyFile string
	Owner          string
	Repository     string
	// httpClient is an internal deterministic-test seam; production callers use direct transport.
	httpClient *http.Client
}

// ActionsOutcomeReader reads one workflow run's job conclusions with a
// repository-scoped actions:read installation token.
type ActionsOutcomeReader struct {
	config ActionsOutcomeReaderConfig
	apiURL *url.URL
	client *http.Client
	tokens *appInstallation
}

// ActionsRunOutcomes is one workflow run's Actions side of a shadow
// comparison.
//
// Attempt travels with the outcomes because a re-run answers the same run ID
// with different conclusions. Evidence that does not name the attempt it came
// from cannot be checked against the run a second reader sees.
type ActionsRunOutcomes struct {
	RunID   int64
	Attempt int
	HeadSHA string
	// Outcomes are in the order GitHub listed the jobs, reported verbatim.
	Outcomes []ActionsOutcome
}

type actionsRunResponse struct {
	ID         int64  `json:"id"`
	RunAttempt int    `json:"run_attempt"`
	HeadSHA    string `json:"head_sha"`
	Status     string `json:"status"`
	Repository struct {
		FullName string `json:"full_name"`
	} `json:"repository"`
}

type actionsRunJobsResponse struct {
	TotalCount int `json:"total_count"`
	Jobs       []struct {
		ID         int64  `json:"id"`
		RunID      int64  `json:"run_id"`
		Name       string `json:"name"`
		HeadSHA    string `json:"head_sha"`
		Status     string `json:"status"`
		Conclusion string `json:"conclusion"`
	} `json:"jobs"`
}

// NewActionsOutcomeReader validates the configuration and loads the App key
// without contacting GitHub.
func NewActionsOutcomeReader(config ActionsOutcomeReaderConfig) (*ActionsOutcomeReader, error) {
	apiURL, err := validAppAPIOrigin(config.APIBaseURL)
	if err != nil {
		return nil, err
	}
	if config.APIBaseURL == "" {
		config.APIBaseURL = "https://api.github.com"
	}
	if config.AppClientID == "" || len(config.AppClientID) > 256 || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) || !repositoryPart.MatchString(config.Repository) {
		return nil, errors.New("invalid GitHub Actions outcome reader configuration")
	}
	privateKey, err := loadAppPrivateKey(config.PrivateKeyFile)
	if err != nil {
		return nil, err
	}
	client := appHTTPClient(config.httpClient)
	return &ActionsOutcomeReader{config: config, apiURL: apiURL, client: client, tokens: &appInstallation{
		apiURL: apiURL, key: privateKey, client: client,
		clientID: config.AppClientID, installationID: config.InstallationID,
		repository: config.Repository, permissions: map[string]string{"actions": "read"},
	}}, nil
}

// Outcomes reads one completed workflow run and returns what each of its jobs
// concluded.
//
// The jobs of ONE attempt are read, the run's latest. A re-run leaves the
// earlier attempt's jobs reachable, and mixing two attempts would report one
// job name twice with two conclusions, which is a pairing nothing can
// attribute.
//
// Job names and conclusions are reported verbatim. A name the correspondence
// cannot use, a conclusion outside the Actions vocabulary and a run listing
// one job name twice are all refused by Collect, which is the one place that
// should decide what a usable pairing is; a reader that tidied them here would
// hide the state an operator needs to see.
//
// A job of the run that has not completed is reported with an empty
// conclusion, which CompareShadowRun grades as indeterminate: the true answer
// for a pairing nobody judged.
func (r *ActionsOutcomeReader) Outcomes(ctx context.Context, runID int64) (ActionsRunOutcomes, error) {
	if r == nil || ctx == nil || runID <= 0 {
		return ActionsRunOutcomes{}, errors.New("invalid workflow run outcome request")
	}
	token, err := r.tokens.accessToken(ctx)
	if err != nil {
		return ActionsRunOutcomes{}, err
	}
	run, err := r.readRun(ctx, token, runID)
	if err != nil {
		return ActionsRunOutcomes{}, err
	}
	outcomes, err := r.readJobs(ctx, token, run)
	if err != nil {
		return ActionsRunOutcomes{}, err
	}
	return ActionsRunOutcomes{RunID: run.ID, Attempt: run.RunAttempt, HeadSHA: run.HeadSHA, Outcomes: outcomes}, nil
}

// readRun fetches the run itself, which is what says which attempt is current,
// which commit the jobs are about, and whether the run has finished.
func (r *ActionsOutcomeReader) readRun(ctx context.Context, token string, runID int64) (actionsRunResponse, error) {
	endpoint := *r.apiURL
	endpoint.Path = path.Join(endpoint.Path, "repos", r.config.Owner, r.config.Repository, "actions", "runs", fmt.Sprint(runID))
	body, err := r.get(ctx, token, endpoint, "workflow run")
	if err != nil {
		return actionsRunResponse{}, err
	}
	var run actionsRunResponse
	if err := json.Unmarshal(body, &run); err != nil {
		return actionsRunResponse{}, fmt.Errorf("%w: unreadable run document", ErrActionsRunUnreadable)
	}
	if run.ID != runID || run.RunAttempt < 1 || !validCommitSHA(run.HeadSHA) ||
		run.Repository.FullName != r.config.Owner+"/"+r.config.Repository {
		return actionsRunResponse{}, fmt.Errorf("%w: run %d does not describe itself", ErrActionsRunUnreadable, runID)
	}
	if run.Status != "completed" {
		return actionsRunResponse{}, fmt.Errorf("%w: run %d is %q", ErrActionsRunIncomplete, runID, run.Status)
	}
	return run, nil
}

// readJobs pages through the run's current attempt.
func (r *ActionsOutcomeReader) readJobs(ctx context.Context, token string, run actionsRunResponse) ([]ActionsOutcome, error) {
	outcomes := []ActionsOutcome{}
	for page := 1; page <= maxActionsJobPages; page++ {
		endpoint := *r.apiURL
		endpoint.Path = path.Join(endpoint.Path, "repos", r.config.Owner, r.config.Repository,
			"actions", "runs", fmt.Sprint(run.ID), "attempts", fmt.Sprint(run.RunAttempt), "jobs")
		query := endpoint.Query()
		query.Set("per_page", "100")
		query.Set("page", fmt.Sprint(page))
		endpoint.RawQuery = query.Encode()
		body, err := r.get(ctx, token, endpoint, "workflow run jobs")
		if err != nil {
			return nil, err
		}
		var jobs actionsRunJobsResponse
		if err := json.Unmarshal(body, &jobs); err != nil || jobs.TotalCount < 0 || len(jobs.Jobs) > 100 {
			return nil, fmt.Errorf("%w: unreadable jobs document", ErrActionsRunUnreadable)
		}
		for _, job := range jobs.Jobs {
			if job.RunID != run.ID {
				return nil, fmt.Errorf("%w: job %d belongs to run %d", ErrActionsRunUnreadable, job.ID, job.RunID)
			}
			outcomes = append(outcomes, ActionsOutcome{Job: job.Name, HeadSHA: job.HeadSHA, Conclusion: job.Conclusion})
		}
		if len(jobs.Jobs) == 0 || page*100 >= jobs.TotalCount {
			return outcomes, nil
		}
	}
	return nil, fmt.Errorf("%w: run %d", ErrActionsRunTooLarge, run.ID)
}

// get performs one bounded read. Every non-200 is ErrActionsRunUnreadable with
// no outcomes, never a partial answer: a collection assembled from part of a
// run is graded as though the missing jobs never ran.
func (r *ActionsOutcomeReader) get(ctx context.Context, token string, endpoint url.URL, what string) ([]byte, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return nil, fmt.Errorf("build GitHub %s request", what)
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := r.client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("read GitHub %s: %w", what, err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%w: %s returned HTTP %d", ErrActionsRunUnreadable, what, response.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maxActionsRunResponse+1))
	if err != nil || len(body) > maxActionsRunResponse {
		return nil, fmt.Errorf("%w: %s response is unreadable or too large", ErrActionsRunUnreadable, what)
	}
	return body, nil
}
