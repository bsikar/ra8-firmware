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
	"regexp"
	"sort"
	"strings"
)

// PlanRequiredChecks works out what branch protection should require, and until
// now the "required today" half of that plan was a list somebody typed. This
// file reads the contexts the repository requires TODAY, so the plan is
// computed against the gate actually holding pull requests.
//
// It only reads. Nothing here changes branch protection: applying the plan is
// the one move in #1481 that can hold every pull request in the repository, and
// it is not made by the code that discovers the gate.

const maxBranchProtectionResponse = 1 << 20

// protectedBranchRef is the rule a branch name must match before it is placed
// in a request path. GitHub accepts far more than this in a ref, but a name
// this reader cannot state plainly is a name whose request path would have to
// be reasoned about, and the branches a gate protects are ordinary ones.
var protectedBranchRef = regexp.MustCompile(`^[A-Za-z0-9](?:[A-Za-z0-9._/-]{0,248}[A-Za-z0-9])?$`)

var (
	// ErrBranchProtectionUnreadable is returned when GitHub did not answer
	// with this branch's protection. It is NOT read as an empty gate: an
	// unprotected branch, a branch that does not exist, and an installation
	// that cannot see administration all answer the same way, and planning
	// against "nothing is required" would propose tearing down a gate this
	// reader never saw.
	ErrBranchProtectionUnreadable = errors.New("GitHub did not return this branch's protection")
	// ErrRequiredCheckSetDisagrees is returned when the protection's
	// checks[] and its deprecated contexts[] name different sets. Two
	// answers to one question is not a gate to plan against.
	ErrRequiredCheckSetDisagrees = errors.New("branch protection's checks and contexts name different sets")
)

// RequiredCheckReaderConfig grants administration:read on one repository, which
// is what reading branch protection takes.
//
// It carries no mode. Reading the gate is the same act whether the deployment
// publishes shadow or authoritative runs, and the mode decides what the plan
// proposes, not what the gate says.
type RequiredCheckReaderConfig struct {
	APIBaseURL     string
	AppClientID    string
	InstallationID int64
	PrivateKeyFile string
	Owner          string
	Repository     string
	// httpClient is an internal deterministic-test seam; production callers use direct transport.
	httpClient *http.Client
}

// RequiredCheckReader reads a protected branch's required status check
// contexts with a repository-scoped administration:read installation token.
type RequiredCheckReader struct {
	config RequiredCheckReaderConfig
	apiURL *url.URL
	client *http.Client
	tokens *appInstallation
}

type branchProtectionResponse struct {
	RequiredStatusChecks *struct {
		Contexts []string `json:"contexts"`
		Checks   []struct {
			Context string `json:"context"`
			AppID   *int64 `json:"app_id"`
		} `json:"checks"`
	} `json:"required_status_checks"`
}

// NewRequiredCheckReader validates the configuration and loads the App key
// without contacting GitHub.
func NewRequiredCheckReader(config RequiredCheckReaderConfig) (*RequiredCheckReader, error) {
	apiURL, err := validAppAPIOrigin(config.APIBaseURL)
	if err != nil {
		return nil, err
	}
	if config.APIBaseURL == "" {
		config.APIBaseURL = "https://api.github.com"
	}
	if config.AppClientID == "" || len(config.AppClientID) > 256 || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) || !repositoryPart.MatchString(config.Repository) {
		return nil, errors.New("invalid GitHub required check reader configuration")
	}
	privateKey, err := loadAppPrivateKey(config.PrivateKeyFile)
	if err != nil {
		return nil, err
	}
	client := appHTTPClient(config.httpClient)
	return &RequiredCheckReader{config: config, apiURL: apiURL, client: client, tokens: &appInstallation{
		apiURL: apiURL, key: privateKey, client: client,
		clientID: config.AppClientID, installationID: config.InstallationID,
		repository: config.Repository, permissions: map[string]string{"administration": "read"},
	}}, nil
}

// RequiredContexts returns the status check contexts branch protection requires
// on this branch, in the order GitHub listed them.
//
// The answer is reported verbatim. A context that is empty, padded or named
// twice is passed through for PlanRequiredChecks to refuse, because one place
// should decide what a usable context is, and a reader that quietly tidied the
// gate would hide exactly the state an operator needs to see.
//
// A protected branch that requires no status checks is an answer, not a
// failure: it returns an empty set and no error. A branch whose protection
// GitHub would not return is ErrBranchProtectionUnreadable.
func (r *RequiredCheckReader) RequiredContexts(ctx context.Context, branch string) ([]string, error) {
	if r == nil || ctx == nil {
		return nil, errors.New("invalid required check read request")
	}
	if !protectedBranchRef.MatchString(branch) || strings.Contains(branch, "..") || strings.Contains(branch, "//") {
		return nil, fmt.Errorf("invalid protected branch name %q", branch)
	}
	token, err := r.tokens.accessToken(ctx)
	if err != nil {
		return nil, err
	}
	endpoint := *r.apiURL
	endpoint.Path = path.Join(endpoint.Path, "repos", r.config.Owner, r.config.Repository, "branches", branch, "protection")
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return nil, errors.New("build branch protection request")
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := r.client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("read branch protection: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%w: HTTP %d", ErrBranchProtectionUnreadable, response.StatusCode)
	}
	var protection branchProtectionResponse
	if err := json.NewDecoder(io.LimitReader(response.Body, maxBranchProtectionResponse)).Decode(&protection); err != nil {
		return nil, fmt.Errorf("%w: unreadable protection document", ErrBranchProtectionUnreadable)
	}
	if protection.RequiredStatusChecks == nil {
		return []string{}, nil
	}
	return reconcileRequiredContexts(*protection.RequiredStatusChecks)
}

// reconcileRequiredContexts picks which of the protection document's two
// answers to report.
//
// GitHub returns the same requirement twice: checks[], which carries the app
// that must post each context, and contexts[], the deprecated flat list. checks
// is the current field and the one reported. contexts is used only when checks
// is absent, and when both are present and disagree the read is refused rather
// than one of them chosen, because a gate that describes itself two ways is not
// one an operator can plan against.
func reconcileRequiredContexts(required struct {
	Contexts []string `json:"contexts"`
	Checks   []struct {
		Context string `json:"context"`
		AppID   *int64 `json:"app_id"`
	} `json:"checks"`
}) ([]string, error) {
	if len(required.Checks) == 0 {
		if len(required.Contexts) == 0 {
			return []string{}, nil
		}
		return append([]string{}, required.Contexts...), nil
	}
	contexts := make([]string, 0, len(required.Checks))
	for _, check := range required.Checks {
		contexts = append(contexts, check.Context)
	}
	if len(required.Contexts) > 0 && !sameContextSet(contexts, required.Contexts) {
		return nil, fmt.Errorf("%w: %d in checks, %d in contexts", ErrRequiredCheckSetDisagrees,
			len(contexts), len(required.Contexts))
	}
	return contexts, nil
}

// sameContextSet compares two listings of one requirement, multiplicity
// included: a context named twice in one and once in the other is a
// disagreement, not a tidying difference.
func sameContextSet(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	leftSorted := append([]string{}, left...)
	rightSorted := append([]string{}, right...)
	sort.Strings(leftSorted)
	sort.Strings(rightSorted)
	for i := range leftSorted {
		if leftSorted[i] != rightSorted[i] {
			return false
		}
	}
	return true
}
