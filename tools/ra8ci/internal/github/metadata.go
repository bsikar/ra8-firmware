// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rsa"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v4"
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
	key    *rsa.PrivateKey
	client *http.Client

	mu        sync.Mutex
	token     string
	tokenTill time.Time
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
	if config.APIBaseURL == "" {
		config.APIBaseURL = "https://api.github.com"
	}
	apiURL, err := url.Parse(config.APIBaseURL)
	if err != nil || apiURL == nil || !strings.EqualFold(apiURL.Scheme, "https") ||
		!strings.EqualFold(apiURL.Hostname(), "api.github.com") ||
		(apiURL.Port() != "" && apiURL.Port() != "443") || apiURL.User != nil ||
		apiURL.Path != "" || apiURL.RawPath != "" || apiURL.RawQuery != "" || apiURL.ForceQuery ||
		apiURL.Fragment != "" || strings.TrimSpace(config.APIBaseURL) != config.APIBaseURL {
		return nil, errors.New("GitHub metadata API base URL must be exactly the public api.github.com HTTPS origin")
	}
	if config.AppClientID == "" || len(config.AppClientID) > 256 || config.InstallationID <= 0 ||
		config.PrivateKeyFile == "" || !ownerName.MatchString(config.Owner) || !repositoryPart.MatchString(config.Repository) {
		return nil, errors.New("invalid GitHub metadata resolver configuration")
	}
	keyInfo, err := os.Lstat(config.PrivateKeyFile)
	if err != nil {
		return nil, fmt.Errorf("stat GitHub App private key: %w", err)
	}
	if !keyInfo.Mode().IsRegular() || keyInfo.Size() < 1 || keyInfo.Size() > maxGitHubPrivateKeyBytes || keyInfo.Mode().Perm()&0077 != 0 {
		return nil, errors.New("GitHub App private key must be a private bounded regular file")
	}
	keyFile, err := os.Open(config.PrivateKeyFile)
	if err != nil {
		return nil, fmt.Errorf("open GitHub App private key: %w", err)
	}
	defer keyFile.Close()
	openedInfo, err := keyFile.Stat()
	if err != nil || !openedInfo.Mode().IsRegular() || !os.SameFile(keyInfo, openedInfo) || openedInfo.Mode().Perm()&0077 != 0 {
		return nil, errors.New("GitHub App private key changed or is not private regular file")
	}
	keyPEM, err := io.ReadAll(io.LimitReader(keyFile, maxGitHubPrivateKeyBytes+1))
	if err != nil || len(keyPEM) < 1 || len(keyPEM) > maxGitHubPrivateKeyBytes {
		return nil, errors.New("GitHub App private key exceeds the bounded file size")
	}
	block, _ := pem.Decode(keyPEM)
	clear(keyPEM)
	if block == nil {
		return nil, errors.New("GitHub App private key is not valid PEM")
	}
	privateKey, err := jwt.ParseRSAPrivateKeyFromPEM(pem.EncodeToMemory(block))
	if err != nil {
		return nil, errors.New("GitHub App private key must contain an RSA private key")
	}
	client := config.httpClient
	if client == nil {
		transport := http.DefaultTransport.(*http.Transport).Clone()
		transport.Proxy = nil
		client = &http.Client{Transport: transport, Timeout: 10 * time.Second}
	} else {
		copyClient := *client
		if copyClient.Transport == nil {
			transport := http.DefaultTransport.(*http.Transport).Clone()
			transport.Proxy = nil
			copyClient.Transport = transport
		} else if transport, ok := copyClient.Transport.(*http.Transport); ok {
			transport = transport.Clone()
			transport.Proxy = nil
			copyClient.Transport = transport
		}
		client = &copyClient
	}
	client.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return &MetadataResolver{config: config, apiURL: apiURL, key: privateKey, client: client}, nil
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

func (r *MetadataResolver) installationToken(ctx context.Context) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.token != "" && time.Until(r.tokenTill) > time.Minute {
		return r.token, nil
	}
	issued := time.Now().Add(-time.Minute)
	claims := jwt.RegisteredClaims{Issuer: r.config.AppClientID,
		IssuedAt: jwt.NewNumericDate(issued), ExpiresAt: jwt.NewNumericDate(issued.Add(8 * time.Minute))}
	appJWT, err := jwt.NewWithClaims(jwt.SigningMethodRS256, claims).SignedString(r.key)
	if err != nil {
		return "", errors.New("sign GitHub App authentication token")
	}
	endpoint := *r.apiURL
	endpoint.Path = path.Join(endpoint.Path, "app", "installations", fmt.Sprint(r.config.InstallationID), "access_tokens")
	payload, _ := json.Marshal(installationTokenRequest{Repositories: []string{r.config.Repository}, Permissions: map[string]string{"actions": "read"}})
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint.String(), strings.NewReader(string(payload)))
	if err != nil {
		return "", errors.New("build GitHub App installation token request")
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+appJWT)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := r.client.Do(request)
	if err != nil {
		return "", fmt.Errorf("create GitHub installation token: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusCreated {
		return "", fmt.Errorf("GitHub installation token endpoint returned HTTP %d", response.StatusCode)
	}
	var token installationTokenResponse
	if err := json.NewDecoder(io.LimitReader(response.Body, maxMetadataResponse)).Decode(&token); err != nil || token.Token == "" || !token.ExpiresAt.After(time.Now().Add(time.Minute)) {
		return "", errors.New("GitHub returned invalid installation token metadata")
	}
	r.token, r.tokenTill = token.Token, token.ExpiresAt
	return r.token, nil
}
