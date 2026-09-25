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
	"sort"
	"strings"
)

// CheckRunPublisher posts one check run and reports the identifier GitHub
// assigned it. A post whose answer never arrived leaves the caller with no
// identifier and no way to tell a request GitHub never saw from one it
// accepted, and the implementation contract is explicit about what must happen
// then: an uncertain Checks API write is reconciled by listing the check runs
// on the commit, not blindly repeated.
//
// Nothing in the tree could list them. This file reads what this plane has
// already published on one commit, so a publisher that does not know whether
// its write landed can go and look instead of posting a second run under a
// name branch protection may one day require.
//
// It only reads, and it reads one commit. Nothing here decides whether a run
// found on the commit answers the one a caller meant to post: that judgement
// needs the intended run beside it and belongs to the slice that wires this
// into publishing. Reporting the commit verbatim and letting one place judge
// it is the division RequiredCheckReader and ActionsOutcomeReader both draw.

const (
	// maxPublishedCheckRunPages bounds the paging, matching the ceiling
	// MetadataResolver and ActionsOutcomeReader already use.
	maxPublishedCheckRunPages = 10
	// maxPublishedCheckRunResponse bounds each response body.
	maxPublishedCheckRunResponse = 1 << 20
)

var (
	// ErrPublishedCheckRunsUnreadable is returned when GitHub did not
	// answer with this commit's check runs. It is NOT read as a commit
	// carrying none: a commit that does not exist, one in another
	// repository and an installation that cannot see checks all answer
	// much the same way, and reading any of them as "nothing published"
	// turns the reconciliation this reader exists for back into the blind
	// repeat it is meant to replace.
	ErrPublishedCheckRunsUnreadable = errors.New("GitHub did not return this commit's check runs")
	// ErrPublishedCheckRunsTooMany is returned when a commit carries more
	// check runs than this reader will page through. A partial listing is
	// refused for the reason above: what is missing from it reads exactly
	// like a run that was never published.
	ErrPublishedCheckRunsTooMany = errors.New("commit carries more check runs than this reader lists")
)

// CheckRunReconcilerConfig grants checks:read on one repository, which is what
// listing a commit's check runs takes.
//
// It carries no mode. Reading what has already been published is the same act
// whether the deployment publishes shadow or authoritative runs, and each run
// found says which of the two it is in its own name.
type CheckRunReconcilerConfig struct {
	APIBaseURL     string
	AppClientID    string
	InstallationID int64
	PrivateKeyFile string
	Owner          string
	Repository     string
	// httpClient is an internal deterministic-test seam; production callers use direct transport.
	httpClient *http.Client
}

// CheckRunReconciler lists a commit's check runs with a repository-scoped
// checks:read installation token.
type CheckRunReconciler struct {
	config CheckRunReconcilerConfig
	apiURL *url.URL
	client *http.Client
	tokens *appInstallation
}

// PublishedCheckRun is one check run already on the commit, under a name this
// plane publishes.
//
// Status and Conclusion are carried verbatim, including the empty conclusion
// of a run that has not finished. A run still queued or in progress is the
// shape an uncertain write most often takes, and refusing to report it would
// hide the one state the caller came to look for.
type PublishedCheckRun struct {
	ID         int64
	Name       string
	Mode       CheckRunMode
	Status     string
	Conclusion string
	Title      string
	// ExternalID is the publisher's own name for the run, carried
	// verbatim and empty for a run that has none: a run posted before
	// this plane wrote the field, or one posted by something else under a
	// name of ours. Both are states an operator has to see.
	ExternalID string
}

// PublishedCheckRuns is what this plane has published on one commit.
type PublishedCheckRuns struct {
	HeadSHA string
	// Runs are sorted by name and then by identifier. The order GitHub
	// pages them in carries no meaning, and a reconciliation read twice
	// has to produce the same document both times.
	Runs []PublishedCheckRun
}

// Named returns every run published under one check run name, in the order
// Runs carries. There may be more than one: GitHub keeps each post as its own
// run, so a repeated write leaves two runs under one name, which is the state
// this reader exists to make visible rather than tidy away.
func (p PublishedCheckRuns) Named(name string) []PublishedCheckRun {
	found := []PublishedCheckRun{}
	for _, run := range p.Runs {
		if run.Name == name {
			found = append(found, run)
		}
	}
	return found
}

type commitCheckRunsResponse struct {
	TotalCount int `json:"total_count"`
	CheckRuns  []struct {
		ID         int64  `json:"id"`
		Name       string `json:"name"`
		HeadSHA    string `json:"head_sha"`
		Status     string `json:"status"`
		Conclusion string `json:"conclusion"`
		ExternalID string `json:"external_id"`
		Output     struct {
			Title string `json:"title"`
		} `json:"output"`
	} `json:"check_runs"`
}

// NewCheckRunReconciler validates the configuration and loads the App key
// without contacting GitHub.
func NewCheckRunReconciler(config CheckRunReconcilerConfig) (*CheckRunReconciler, error) {
	apiURL, err := validAppAPIOrigin(config.APIBaseURL)
	if err != nil {
		return nil, err
	}
	if config.APIBaseURL == "" {
		config.APIBaseURL = "https://api.github.com"
	}
	if config.AppClientID == "" || len(config.AppClientID) > 256 || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) || !repositoryPart.MatchString(config.Repository) {
		return nil, errors.New("invalid GitHub check run reconciler configuration")
	}
	privateKey, err := loadAppPrivateKey(config.PrivateKeyFile)
	if err != nil {
		return nil, err
	}
	client := appHTTPClient(config.httpClient)
	return &CheckRunReconciler{config: config, apiURL: apiURL, client: client, tokens: &appInstallation{
		apiURL: apiURL, key: privateKey, client: client,
		clientID: config.AppClientID, installationID: config.InstallationID,
		repository: config.Repository, permissions: map[string]string{"checks": "read"},
	}}, nil
}

// PublishedRuns lists the check runs this plane has already published on one
// commit.
//
// The commit is named by its full SHA and nothing else. A branch or tag would
// answer about whatever it points at when the listing is made, and a publish
// reconciled against another commit's runs is worse than no reconciliation at
// all: it would report a run that was never posted for the commit in hand.
//
// A run under a name this plane does not publish is left out. The Actions
// checks on the commit are read by ActionsOutcomeReader from the run that
// produced them, and a foreign name is not evidence about what this plane
// wrote. A name inside either ra8ci namespace is reported whoever posted it,
// because a run occupying a name branch protection may require is exactly the
// collision an operator has to see.
func (r *CheckRunReconciler) PublishedRuns(ctx context.Context, headSHA string) (PublishedCheckRuns, error) {
	if r == nil || ctx == nil {
		return PublishedCheckRuns{}, errors.New("invalid published check run request")
	}
	if !validCommitSHA(headSHA) {
		return PublishedCheckRuns{}, fmt.Errorf("%w: %q", ErrInvalidCheckRunSHA, headSHA)
	}
	commit := strings.ToLower(headSHA)
	token, err := r.tokens.accessToken(ctx)
	if err != nil {
		return PublishedCheckRuns{}, err
	}
	runs := []PublishedCheckRun{}
	for page := 1; page <= maxPublishedCheckRunPages; page++ {
		listed, total, err := r.readPage(ctx, token, commit, page)
		if err != nil {
			return PublishedCheckRuns{}, err
		}
		runs = append(runs, listed...)
		if total <= page*100 {
			sort.Slice(runs, func(i, j int) bool {
				if runs[i].Name != runs[j].Name {
					return runs[i].Name < runs[j].Name
				}
				return runs[i].ID < runs[j].ID
			})
			return PublishedCheckRuns{HeadSHA: commit, Runs: runs}, nil
		}
	}
	return PublishedCheckRuns{}, fmt.Errorf("%w: %s", ErrPublishedCheckRunsTooMany, commit)
}

// readPage reads one page of the commit's check runs and keeps the ones this
// plane publishes.
func (r *CheckRunReconciler) readPage(ctx context.Context, token, commit string, page int) ([]PublishedCheckRun, int, error) {
	endpoint := *r.apiURL
	endpoint.Path = path.Join(endpoint.Path, "repos", r.config.Owner, r.config.Repository, "commits", commit, "check-runs")
	query := endpoint.Query()
	query.Set("per_page", "100")
	query.Set("page", fmt.Sprint(page))
	endpoint.RawQuery = query.Encode()
	body, err := r.get(ctx, token, endpoint)
	if err != nil {
		return nil, 0, err
	}
	var listing commitCheckRunsResponse
	if err := json.Unmarshal(body, &listing); err != nil || listing.TotalCount < 0 || len(listing.CheckRuns) > 100 {
		return nil, 0, fmt.Errorf("%w: unreadable check run listing", ErrPublishedCheckRunsUnreadable)
	}
	if len(listing.CheckRuns) == 0 {
		return nil, page * 100, nil
	}
	kept := []PublishedCheckRun{}
	for _, run := range listing.CheckRuns {
		// A listing that answers about another commit cannot be
		// reconciled against this one, the same self-description check
		// the workflow run reader makes.
		if !strings.EqualFold(run.HeadSHA, commit) {
			return nil, 0, fmt.Errorf("%w: run %d is on %s", ErrPublishedCheckRunsUnreadable, run.ID, run.HeadSHA)
		}
		var mode CheckRunMode
		switch ownerOfContext(run.Name) {
		case contextShadow:
			mode = ModeShadow
		case contextAuthoritative:
			mode = ModeAuthoritative
		default:
			continue
		}
		kept = append(kept, PublishedCheckRun{
			ID: run.ID, Name: run.Name, Mode: mode,
			Status: run.Status, Conclusion: run.Conclusion, Title: run.Output.Title,
			ExternalID: run.ExternalID,
		})
	}
	return kept, listing.TotalCount, nil
}

// get performs one bounded read. Every non-200 is ErrPublishedCheckRunsUnreadable
// with no runs, never a partial answer.
func (r *CheckRunReconciler) get(ctx context.Context, token string, endpoint url.URL) ([]byte, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return nil, errors.New("build GitHub check run listing request")
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := r.client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("read GitHub check run listing: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%w: listing returned HTTP %d", ErrPublishedCheckRunsUnreadable, response.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maxPublishedCheckRunResponse+1))
	if err != nil || len(body) > maxPublishedCheckRunResponse {
		return nil, fmt.Errorf("%w: unreadable check run listing", ErrPublishedCheckRunsUnreadable)
	}
	return body, nil
}
