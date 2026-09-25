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

// #1481 holds the required-check move until this plane's conclusions have been
// "compared against Actions over representative pull requests". Everything
// that gathers that evidence is addressed by commit: ActionsOutcomeReader
// reads a workflow run, CheckRunReconciler lists a commit's check runs,
// AccumulateShadowEvidence counts graded commits. Nothing could turn a pull
// request into the commit its evidence is about, so the operator gathering it
// reads head SHAs out of the web interface and types them into documents.
//
// This file is that hop, and only that hop. It answers what one pull request
// is at right now, and says enough about the pull request for somebody
// weighing the evidence to know what they are looking at: whether it merged,
// what it targets, and whether the head came from a fork.
//
// It reads one pull request and judges nothing else. It does not list the
// runs on the head it reports: that read is actions:read and belongs beside
// the reader that already holds it, and keeping the two apart is what lets
// this token stay at pull_requests:read alone.

var (
	// ErrPullRequestUnreadable is returned when GitHub did not answer with
	// this pull request. It is NOT read as a pull request with no head: a
	// number that does not exist, one in another repository and an
	// installation that cannot see pull requests all answer much the same
	// way, and an empty head would send every downstream read at the zero
	// commit. Same reasoning as ErrActionsRunUnreadable.
	ErrPullRequestUnreadable = errors.New("GitHub did not return this pull request")
	// ErrPullRequestHeadUnknown is returned for a pull request GitHub
	// answered for whose head this reader cannot use: no SHA, or one that
	// is not a commit. A pull request whose head was force-pushed away
	// answers this rather than a commit nothing can be read about.
	ErrPullRequestHeadUnknown = errors.New("pull request does not name a usable head commit")
)

// PullRequestHeadReaderConfig grants pull_requests:read on one repository,
// which is what reading a pull request takes.
//
// It carries no mode. Which commit a pull request is at is the same fact
// whether the deployment publishes shadow or authoritative check runs.
type PullRequestHeadReaderConfig struct {
	APIBaseURL     string
	AppClientID    string
	InstallationID int64
	PrivateKeyFile string
	Owner          string
	Repository     string
	// httpClient is an internal deterministic-test seam; production callers use direct transport.
	httpClient *http.Client
}

// PullRequestHeadReader reads one pull request's head with a
// repository-scoped pull_requests:read installation token.
type PullRequestHeadReader struct {
	config PullRequestHeadReaderConfig
	apiURL *url.URL
	client *http.Client
	tokens *appInstallation
}

// PullRequestHead is what one pull request is at, and enough about the pull
// request to weigh the evidence gathered on it.
//
// HeadSHA is the head as of this read. A pull request's head moves: a later
// read answers a different commit for the same number, so evidence recorded
// against a number rather than against the commit it was at cannot be checked
// by a second reader. Everything downstream is addressed by HeadSHA for that
// reason, and the number is carried alongside only to say where it came from.
type PullRequestHead struct {
	Number  int
	HeadSHA string
	// BaseRef is the branch the pull request targets, reported so evidence
	// for a gate on one branch is distinguishable from evidence gathered
	// against another.
	BaseRef string
	// State is GitHub's word for the pull request, verbatim.
	State string
	// Merged says whether it landed. A merged pull request's head is a
	// commit the repository accepted; a closed unmerged one's is not.
	Merged bool
	// FromFork says the head commit came from a repository other than this
	// one. Durable message and job policy holds that fork pull requests
	// and untrusted refs cannot obtain board or provisioner authority, so
	// evidence gathered on one is evidence about code this repository did
	// not control, and an operator moving a merge gate has to be able to
	// see which side of that line it came from.
	FromFork bool
	// HeadRepository is the full name of the repository the head lives in,
	// reported verbatim and empty when GitHub sent none (a deleted fork).
	HeadRepository string
}

type pullRequestResponse struct {
	Number int    `json:"number"`
	State  string `json:"state"`
	Merged bool   `json:"merged"`
	Head   struct {
		SHA  string `json:"sha"`
		Repo *struct {
			FullName string `json:"full_name"`
		} `json:"repo"`
	} `json:"head"`
	Base struct {
		Ref  string `json:"ref"`
		Repo struct {
			FullName string `json:"full_name"`
		} `json:"repo"`
	} `json:"base"`
}

// maxPullRequestResponse bounds the response body. One pull request document
// is a few kilobytes even with a long description, so this is room to spare
// and still a refusal rather than an unbounded read.
const maxPullRequestResponse = 1 << 20

// NewPullRequestHeadReader validates the configuration and loads the App key
// without contacting GitHub.
func NewPullRequestHeadReader(config PullRequestHeadReaderConfig) (*PullRequestHeadReader, error) {
	apiURL, err := validAppAPIOrigin(config.APIBaseURL)
	if err != nil {
		return nil, err
	}
	if config.APIBaseURL == "" {
		config.APIBaseURL = "https://api.github.com"
	}
	if config.AppClientID == "" || len(config.AppClientID) > 256 || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) || !repositoryPart.MatchString(config.Repository) {
		return nil, errors.New("invalid GitHub pull request head reader configuration")
	}
	privateKey, err := loadAppPrivateKey(config.PrivateKeyFile)
	if err != nil {
		return nil, err
	}
	client := appHTTPClient(config.httpClient)
	return &PullRequestHeadReader{config: config, apiURL: apiURL, client: client, tokens: &appInstallation{
		apiURL: apiURL, key: privateKey, client: client,
		clientID: config.AppClientID, installationID: config.InstallationID,
		repository: config.Repository, permissions: map[string]string{"pull_requests": "read"},
	}}, nil
}

// Head reads one pull request and returns the commit it is at.
//
// An open pull request is answered as readily as a merged one. Which pull
// requests are representative is the operator's judgement, stated when they
// choose the numbers; a reader that answered only for merged ones would be
// making that judgement quietly, and the fields it reports are there so the
// judgement can be made on something.
//
// A pull request GitHub answered for whose head is a fork is answered too,
// with FromFork set. Refusing it would hide from the evidence exactly the
// pull requests the admission policy treats as untrusted.
func (r *PullRequestHeadReader) Head(ctx context.Context, number int) (PullRequestHead, error) {
	if r == nil || ctx == nil || number <= 0 {
		return PullRequestHead{}, errors.New("invalid pull request head request")
	}
	token, err := r.tokens.accessToken(ctx)
	if err != nil {
		return PullRequestHead{}, err
	}
	endpoint := *r.apiURL
	endpoint.Path = path.Join(endpoint.Path, "repos", r.config.Owner, r.config.Repository, "pulls", fmt.Sprint(number))
	body, err := r.get(ctx, token, endpoint)
	if err != nil {
		return PullRequestHead{}, err
	}
	var pull pullRequestResponse
	if err := json.Unmarshal(body, &pull); err != nil {
		return PullRequestHead{}, fmt.Errorf("%w: unreadable pull request document", ErrPullRequestUnreadable)
	}
	repository := r.config.Owner + "/" + r.config.Repository
	if pull.Number != number || pull.State == "" || pull.Base.Repo.FullName != repository {
		return PullRequestHead{}, fmt.Errorf("%w: pull request %d does not describe itself", ErrPullRequestUnreadable, number)
	}
	if !validCommitSHA(pull.Head.SHA) {
		return PullRequestHead{}, fmt.Errorf("%w: pull request %d", ErrPullRequestHeadUnknown, number)
	}
	head := PullRequestHead{
		Number:  pull.Number,
		HeadSHA: pull.Head.SHA,
		BaseRef: pull.Base.Ref,
		State:   pull.State,
		Merged:  pull.Merged,
	}
	// A head repository GitHub did not send is a fork that has since been
	// deleted. Reading the absence as "same repository" would call the one
	// case nobody can go and look at the trusted one.
	if pull.Head.Repo == nil {
		head.FromFork = true
		return head, nil
	}
	head.HeadRepository = pull.Head.Repo.FullName
	head.FromFork = pull.Head.Repo.FullName != repository
	return head, nil
}

// get performs one bounded read. Every non-200 is ErrPullRequestUnreadable
// with no head, never a partial answer.
func (r *PullRequestHeadReader) get(ctx context.Context, token string, endpoint url.URL) ([]byte, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return nil, errors.New("build GitHub pull request request")
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := r.client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("read GitHub pull request: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%w: pull request read returned HTTP %d", ErrPullRequestUnreadable, response.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maxPullRequestResponse+1))
	if err != nil || len(body) > maxPullRequestResponse {
		return nil, fmt.Errorf("%w: pull request response is unreadable or too large", ErrPullRequestUnreadable)
	}
	return body, nil
}
