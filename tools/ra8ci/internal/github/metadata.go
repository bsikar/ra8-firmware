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
	"strings"
	"time"
)

const (
	maxMetadataResponse = 1 << 20
	githubAPIVersion    = "2022-11-28"
)

var repositoryPart = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,100}$`)

// JobMetadata is obtained independently from GitHub's Actions API and is used
// to bind a queued scale-set job to a precise workflow attempt and source SHA.
type JobMetadata struct {
	WorkflowAttempt int
	CommitSHA       string
	JobID           string
	WorkflowRunID   int64
	Repository      string
}

// MetadataConfig grants read-only Actions access to one repository. APIBaseURL
// is restricted to the public https://api.github.com origin.
type MetadataConfig struct {
	APIBaseURL     string
	AppClientID    string
	InstallationID int64
	PrivateKeyFile string
	Owner          string
	Repository     string
	// httpClient is an internal deterministic-test seam; production callers use direct transport.
	httpClient *http.Client
}

// MetadataResolver fetches trusted workflow-run metadata with a repository-
// scoped, read-only GitHub App installation token.
type MetadataResolver struct {
	config MetadataConfig
	apiURL *url.URL
	client *http.Client
	tokens *appInstallation
}

type installationTokenRequest struct {
	Repositories []string          `json:"repositories"`
	Permissions  map[string]string `json:"permissions"`
}

type installationTokenResponse struct {
	Token     string    `json:"token"`
	ExpiresAt time.Time `json:"expires_at"`
}

type workflowRunResponse struct {
	ID         int64  `json:"id"`
	RunAttempt int    `json:"run_attempt"`
	HeadSHA    string `json:"head_sha"`
	HeadBranch string `json:"head_branch"`
	Path       string `json:"path"`
	Event      string `json:"event"`
	Repository struct {
		FullName string `json:"full_name"`
	} `json:"repository"`
}

type workflowJobsResponse struct {
	TotalCount int `json:"total_count"`
	Jobs       []struct {
		ID         int64  `json:"id"`
		RunID      int64  `json:"run_id"`
		Name       string `json:"name"`
		HeadSHA    string `json:"head_sha"`
		HeadBranch string `json:"head_branch"`
	} `json:"jobs"`
}

// NewMetadataResolver validates and loads the App key without contacting GitHub.
func NewMetadataResolver(config MetadataConfig) (*MetadataResolver, error) {
	apiURL, err := validAppAPIOrigin(config.APIBaseURL)
	if err != nil {
		return nil, err
	}
	if config.APIBaseURL == "" {
		config.APIBaseURL = "https://api.github.com"
	}
	if config.AppClientID == "" || len(config.AppClientID) > 256 || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) || !repositoryPart.MatchString(config.Repository) {
		return nil, errors.New("invalid GitHub metadata resolver configuration")
	}
	privateKey, err := loadAppPrivateKey(config.PrivateKeyFile)
	if err != nil {
		return nil, err
	}
	client := appHTTPClient(config.httpClient)
	return &MetadataResolver{config: config, apiURL: apiURL, client: client, tokens: &appInstallation{
		apiURL: apiURL, key: privateKey, client: client,
		clientID: config.AppClientID, installationID: config.InstallationID,
		repository: config.Repository, permissions: map[string]string{"actions": "read"},
	}}, nil
}

// Resolve fetches the workflow run and checks its repo/ref/event/source against
// the scale-set message before returning immutable run metadata.
func (r *MetadataResolver) Resolve(ctx context.Context, job Job) (JobMetadata, error) {
	if r == nil || ctx == nil || job.Owner != r.config.Owner || job.Repository != r.config.Repository ||
		job.WorkflowRunID <= 0 || job.JobID == "" || len(job.JobID) > 256 || job.DisplayName == "" || job.EventName == "" {
		return JobMetadata{}, errors.New("invalid or foreign scale-set job metadata request")
	}
	workflowPath, branch, ok := parseWorkflowRef(job.WorkflowRef, job.Owner, job.Repository)
	if !ok {
		return JobMetadata{}, errors.New("scale-set workflow reference is not an exact branch ref")
	}
	token, err := r.installationToken(ctx)
	if err != nil {
		return JobMetadata{}, err
	}
	endpoint := *r.apiURL
	endpoint.Path = path.Join(endpoint.Path, "repos", job.Owner, job.Repository, "actions", "runs", fmt.Sprint(job.WorkflowRunID))
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return JobMetadata{}, errors.New("build GitHub workflow metadata request")
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := r.client.Do(request)
	if err != nil {
		return JobMetadata{}, fmt.Errorf("fetch GitHub workflow metadata: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return JobMetadata{}, fmt.Errorf("GitHub workflow metadata returned HTTP %d", response.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maxMetadataResponse+1))
	if err != nil || len(body) > maxMetadataResponse {
		return JobMetadata{}, errors.New("GitHub workflow metadata response is unreadable or too large")
	}
	var run workflowRunResponse
	if err := json.Unmarshal(body, &run); err != nil {
		return JobMetadata{}, errors.New("GitHub workflow metadata response is invalid")
	}
	if run.ID != job.WorkflowRunID || run.RunAttempt < 1 || run.Repository.FullName != job.Owner+"/"+job.Repository ||
		run.Event != job.EventName || run.HeadBranch != branch || path.Clean(strings.SplitN(run.Path, "@", 2)[0]) != workflowPath ||
		!validCommitSHA(run.HeadSHA) {
		return JobMetadata{}, errors.New("GitHub workflow metadata does not match the scale-set job")
	}
	jobFound, err := r.workflowJobExists(ctx, token, job, run, branch)
	if err != nil {
		return JobMetadata{}, err
	}
	if !jobFound {
		return JobMetadata{}, errors.New("GitHub workflow attempt does not contain the scale-set job")
	}
	return JobMetadata{WorkflowAttempt: run.RunAttempt, CommitSHA: strings.ToLower(run.HeadSHA),
		JobID: job.JobID, WorkflowRunID: run.ID, Repository: run.Repository.FullName}, nil
}

func (r *MetadataResolver) workflowJobExists(ctx context.Context, token string, job Job, run workflowRunResponse, branch string) (bool, error) {
	for page := 1; page <= 10; page++ {
		endpoint := *r.apiURL
		endpoint.Path = path.Join(endpoint.Path, "repos", job.Owner, job.Repository, "actions", "runs", fmt.Sprint(run.ID), "attempts", fmt.Sprint(run.RunAttempt), "jobs")
		query := endpoint.Query()
		query.Set("per_page", "100")
		query.Set("page", fmt.Sprint(page))
		endpoint.RawQuery = query.Encode()
		request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
		if err != nil {
			return false, errors.New("build GitHub workflow jobs request")
		}
		request.Header.Set("Accept", "application/vnd.github+json")
		request.Header.Set("Authorization", "Bearer "+token)
		request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
		response, err := r.client.Do(request)
		if err != nil {
			return false, fmt.Errorf("fetch GitHub workflow jobs: %w", err)
		}
		if response.StatusCode != http.StatusOK {
			response.Body.Close()
			return false, fmt.Errorf("GitHub workflow jobs returned HTTP %d", response.StatusCode)
		}
		body, readErr := io.ReadAll(io.LimitReader(response.Body, maxMetadataResponse+1))
		response.Body.Close()
		if readErr != nil || len(body) > maxMetadataResponse {
			return false, errors.New("GitHub workflow jobs response is unreadable or too large")
		}
		var jobs workflowJobsResponse
		if err := json.Unmarshal(body, &jobs); err != nil || jobs.TotalCount < 0 || jobs.TotalCount > 1000 || len(jobs.Jobs) > 100 {
			return false, errors.New("GitHub workflow jobs response is invalid or exceeds supported bounds")
		}
		for _, candidate := range jobs.Jobs {
			if candidate.ID > 0 && candidate.RunID == run.ID && candidate.Name == job.DisplayName &&
				candidate.HeadBranch == branch && strings.EqualFold(candidate.HeadSHA, run.HeadSHA) {
				return true, nil
			}
		}
		if len(jobs.Jobs) == 0 || page*100 >= jobs.TotalCount {
			return false, nil
		}
	}
	return false, errors.New("GitHub workflow attempt has more than 1000 jobs")
}

func parseWorkflowRef(ref, owner, repository string) (string, string, bool) {
	prefix := owner + "/" + repository + "/"
	if !strings.HasPrefix(ref, prefix) {
		return "", "", false
	}
	parts := strings.SplitN(strings.TrimPrefix(ref, prefix), "@", 2)
	if len(parts) != 2 || !strings.HasPrefix(parts[0], ".github/workflows/") ||
		strings.Contains(parts[0], "..") || !strings.HasPrefix(parts[1], "refs/heads/") {
		return "", "", false
	}
	branch := strings.TrimPrefix(parts[1], "refs/heads/")
	if branch == "" || strings.ContainsAny(branch, " \t\r\n") || path.Clean(parts[0]) != parts[0] {
		return "", "", false
	}
	return parts[0], branch, true
}

func validCommitSHA(value string) bool {
	if len(value) != 40 {
		return false
	}
	for _, char := range value {
		if !(char >= '0' && char <= '9' || char >= 'a' && char <= 'f' || char >= 'A' && char <= 'F') {
			return false
		}
	}
	return true
}

// installationToken returns the resolver's read-only Actions token. The
// minting itself lives in appInstallation so the check-run publisher's
// checks:write token is minted the same way and differs only in permissions.
func (r *MetadataResolver) installationToken(ctx context.Context) (string, error) {
	return r.tokens.accessToken(ctx)
}
