// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"path"
	"unicode/utf8"
)

// CheckRunPublisher posts one completed check run per catalog task through the
// GitHub App. #1481 runs this in shadow mode first, and the publisher enforces
// that from its configuration rather than trusting each caller: an
// authoritative run is refused unless the deployment asked for one.

const maxCheckRunResponse = 1 << 20

// maxCheckRunSummary is GitHub's ceiling for a check run's output summary, in
// characters. A body over it is refused by the API with a status code and no
// explanation of which field was at fault, so the bound is applied here where
// the field has a name.
const maxCheckRunSummary = 65535

var (
	// ErrCheckRunModeNotPermitted is returned when a run is published in a
	// mode this publisher was not configured for.
	ErrCheckRunModeNotPermitted = errors.New("publisher is not configured for this check run mode")
	// ErrCheckRunRejected is returned when GitHub refused the check run.
	ErrCheckRunRejected = errors.New("GitHub refused the check run")
	// ErrCheckRunOutputUnusable is returned when the output a caller asked
	// to publish is one GitHub will not accept: an empty summary, or a
	// summary or title past the API's ceiling. It is refused before an
	// installation token is minted, so a body GitHub would reject never
	// costs a token or reaches the network.
	ErrCheckRunOutputUnusable = errors.New("check run output is unusable")
)

// CheckRunPublisherConfig grants checks:write on one repository.
//
// Mode is the strongest mode this publisher may post. A shadow publisher
// refuses an authoritative run, so moving ra8ci onto the merge gate is a
// deployment change that is visible in configuration, not a caller passing a
// different argument.
type CheckRunPublisherConfig struct {
	APIBaseURL     string
	AppClientID    string
	InstallationID int64
	PrivateKeyFile string
	Owner          string
	Repository     string
	Mode           CheckRunMode
	// httpClient is an internal deterministic-test seam; production callers use direct transport.
	httpClient *http.Client
}

// CheckRunPublisher publishes check runs with a repository-scoped
// checks:write installation token.
type CheckRunPublisher struct {
	config CheckRunPublisherConfig
	apiURL *url.URL
	client *http.Client
	tokens *appInstallation
}

type checkRunRequest struct {
	Name       string `json:"name"`
	HeadSHA    string `json:"head_sha"`
	Status     string `json:"status"`
	Conclusion string `json:"conclusion"`
	// ExternalID is this plane's own name for the run, which a later
	// reconciliation recomputes from the run in hand to tell a run this
	// deployment posted from one that merely shares the name.
	ExternalID string `json:"external_id"`
	Output     struct {
		Title   string `json:"title"`
		Summary string `json:"summary"`
	} `json:"output"`
}

type checkRunResponse struct {
	ID         int64  `json:"id"`
	Name       string `json:"name"`
	HeadSHA    string `json:"head_sha"`
	Status     string `json:"status"`
	Conclusion string `json:"conclusion"`
	ExternalID string `json:"external_id"`
}

// NewCheckRunPublisher validates the configuration and loads the App key
// without contacting GitHub.
func NewCheckRunPublisher(config CheckRunPublisherConfig) (*CheckRunPublisher, error) {
	apiURL, err := validAppAPIOrigin(config.APIBaseURL)
	if err != nil {
		return nil, err
	}
	if config.APIBaseURL == "" {
		config.APIBaseURL = "https://api.github.com"
	}
	if config.Mode != ModeShadow && config.Mode != ModeAuthoritative {
		return nil, fmt.Errorf("%w: %s", ErrInvalidCheckRunMode, config.Mode)
	}
	if config.AppClientID == "" || len(config.AppClientID) > 256 || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) || !repositoryPart.MatchString(config.Repository) {
		return nil, errors.New("invalid GitHub check run publisher configuration")
	}
	privateKey, err := loadAppPrivateKey(config.PrivateKeyFile)
	if err != nil {
		return nil, err
	}
	client := appHTTPClient(config.httpClient)
	return &CheckRunPublisher{config: config, apiURL: apiURL, client: client, tokens: &appInstallation{
		apiURL: apiURL, key: privateKey, client: client,
		clientID: config.AppClientID, installationID: config.InstallationID,
		repository: config.Repository, permissions: map[string]string{"checks": "write"},
	}}, nil
}

// Publish posts one completed check run and returns the identifier GitHub
// assigned it.
//
// An authoritative run is refused before any request is built when this
// publisher is configured for shadow mode, so a caller cannot move ra8ci onto
// the merge gate by passing a different mode.
func (p *CheckRunPublisher) Publish(ctx context.Context, run TaskCheckRun, summary string) (int64, error) {
	if p == nil || ctx == nil {
		return 0, errors.New("invalid check run publish request")
	}
	if run.Name == "" || !validCommitSHA(run.HeadSHA) || run.Status != "completed" || run.Conclusion == "" {
		return 0, errors.New("incomplete check run")
	}
	if run.Mode == ModeAuthoritative && p.config.Mode == ModeShadow {
		return 0, fmt.Errorf("%w: publisher is %s", ErrCheckRunModeNotPermitted, p.config.Mode)
	}
	if run.Mode != ModeShadow && run.Mode != ModeAuthoritative {
		return 0, fmt.Errorf("%w: %s", ErrInvalidCheckRunMode, run.Mode)
	}
	if err := checkPublishableOutput(run.Title, summary); err != nil {
		return 0, err
	}
	token, err := p.tokens.accessToken(ctx)
	if err != nil {
		return 0, err
	}
	externalID, err := CheckRunExternalID(run)
	if err != nil {
		return 0, err
	}
	var body checkRunRequest
	body.Name, body.HeadSHA, body.Status, body.Conclusion = run.Name, run.HeadSHA, run.Status, run.Conclusion
	body.ExternalID = externalID
	body.Output.Title, body.Output.Summary = run.Title, summary
	payload, err := json.Marshal(body)
	if err != nil {
		return 0, errors.New("encode check run request")
	}
	endpoint := *p.apiURL
	endpoint.Path = path.Join(endpoint.Path, "repos", p.config.Owner, p.config.Repository, "check-runs")
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint.String(), bytes.NewReader(payload))
	if err != nil {
		return 0, errors.New("build check run request")
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := p.client.Do(request)
	if err != nil {
		return 0, fmt.Errorf("post check run: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusCreated {
		return 0, fmt.Errorf("%w: HTTP %d", ErrCheckRunRejected, response.StatusCode)
	}
	var created checkRunResponse
	if err := json.NewDecoder(io.LimitReader(response.Body, maxCheckRunResponse)).Decode(&created); err != nil || created.ID <= 0 {
		return 0, errors.New("GitHub returned invalid check run metadata")
	}
	// GitHub is the record of what was published. A response describing a
	// different run than the one posted is not a success to report, because
	// a comparison against Actions would then be made against the wrong run.
	if err := checkCreatedRunIsTheRunPosted(created, run, externalID); err != nil {
		return 0, err
	}
	return created.ID, nil
}

// CheckPublishableOutput reports whether an output body is one GitHub will
// accept, so a caller assembling a document of runs can refuse the whole
// document before any single run of it is posted.
//
// The publisher applies this rule to every run it posts, and that is the last
// place it can be applied: by then the runs before it in the document are
// already on the commit, and a check run cannot be taken back. Exporting it
// keeps the rule in one definition rather than restating GitHub's ceilings
// wherever a document is built.
func CheckPublishableOutput(title, summary string) error {
	return checkPublishableOutput(title, summary)
}

// checkPublishableOutput holds the output body to what GitHub will accept,
// before anything is built and before a token is minted.
//
// The summary is the only place a check run explains itself, so an over-long
// one is refused and never cut: a published body that stops mid-account reads
// as the whole account, and the caller that assembled it is the one that knows
// what to drop. The report a reconciliation writes excerpts a summary instead,
// but that excerpt announces itself with a field beside it; a published run
// carries no such field.
//
// Both ceilings are counted in characters, the unit GitHub states them in. A
// byte count would refuse a legal summary that happens to be written in a
// script whose characters take more than one byte.
func checkPublishableOutput(title, summary string) error {
	if summary == "" {
		return fmt.Errorf("%w: a check run is published with a summary", ErrCheckRunOutputUnusable)
	}
	if length := utf8.RuneCountInString(summary); length > maxCheckRunSummary {
		return fmt.Errorf("%w: summary is %d characters, GitHub accepts %d",
			ErrCheckRunOutputUnusable, length, maxCheckRunSummary)
	}
	if length := utf8.RuneCountInString(title); length > maxCheckRunTitle {
		return fmt.Errorf("%w: title is %d characters, GitHub accepts %d",
			ErrCheckRunOutputUnusable, length, maxCheckRunTitle)
	}
	return nil
}
