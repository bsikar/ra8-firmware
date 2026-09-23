// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package runnerclock finds impossible step-time orderings in GitHub Actions.
package runnerclock

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode"
)

const (
	apiBaseURL       = "https://api.github.com"
	apiVersion       = "2022-11-28"
	maxBody          = 8 << 20
	defaultRepo      = "bsikar/ra8-firmware"
	defaultRuns      = 40
	ciRuns           = 60
	overlapTolerance = 5 * time.Second
)

var repoPattern = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$`)

type step struct {
	Name        string `json:"name"`
	StartedAt   string `json:"started_at"`
	CompletedAt string `json:"completed_at"`
}

type job struct {
	Name       string `json:"name"`
	RunnerName string `json:"runner_name"`
	Steps      []step `json:"steps"`
}

type run struct {
	ID         int64  `json:"id"`
	Name       string `json:"name"`
	RunStarted string `json:"run_started_at"`
	CreatedAt  string `json:"created_at"`
}

type runList struct {
	Runs []run `json:"workflow_runs"`
}

type jobList struct {
	Jobs []job `json:"jobs"`
}

type finding struct {
	kind, step, detail, runner, job, workflow string
	seconds                                   float64
	at                                        time.Time
}

type actionsAPI struct {
	base   *url.URL
	client *http.Client
	token  string
}

// Run executes the runner-clock scan. --ci-scan reproduces the workflow's
// RA8_CLOCK_SCAN_RUNS default (60) while keeping the task definition static.
func Run(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || stdout == nil || stderr == nil {
		if stderr != nil {
			fmt.Fprintln(stderr, "runner-clock: invalid input")
		}
		return 2
	}
	flags := flag.NewFlagSet("runner-clock", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	repo := flags.String("repo", defaultRepo, "owner/repository")
	runs := flags.Int("runs", defaultRuns, "completed runs to scan")
	hours := flags.Int("hours", 0, "only runs started within N hours")
	self := flags.Bool("selftest", false, "prove the detector, then exit")
	ciScan := flags.Bool("ci-scan", false, "workflow scan with RA8_CLOCK_SCAN_RUNS default")
	if err := flags.Parse(args); err != nil || flags.NArg() != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci runner-clock [--repo OWNER/REPO] [--runs N] [--hours N] | --selftest")
		return 2
	}
	hoursProvided := false
	flags.Visit(func(item *flag.Flag) {
		if item.Name == "hours" {
			hoursProvided = true
		}
	})
	if *self {
		if *ciScan || *repo != defaultRepo || *runs != defaultRuns || hoursProvided {
			fmt.Fprintln(stderr, "runner-clock: --selftest cannot be combined with scan options")
			return 2
		}
		if selfTest(stdout) {
			return 0
		}
		return 1
	}
	limit := *runs
	if *ciScan {
		if *repo != defaultRepo || *runs != defaultRuns || hoursProvided {
			fmt.Fprintln(stderr, "runner-clock: --ci-scan cannot be combined with other scan options")
			return 2
		}
		var err error
		limit, err = ciRunLimit(os.Getenv("RA8_CLOCK_SCAN_RUNS"))
		if err != nil {
			fmt.Fprintln(stderr, "runner-clock:", err)
			return 2
		}
	}
	if limit < 1 {
		fmt.Fprintln(stderr, "runner-clock: --runs must be at least 1; a scan of nothing proves nothing.")
		return 2
	}
	if *hours < 0 {
		fmt.Fprintln(stderr, "runner-clock: --hours cannot be negative")
		return 2
	}
	if !validRepo(*repo) {
		fmt.Fprintln(stderr, "runner-clock: --repo must be owner/repository using GitHub name characters")
		return 2
	}
	token, err := loadToken(ctx)
	if err != nil {
		fmt.Fprintln(stderr, "runner-clock:", err)
		return 2
	}
	api, err := newActionsAPI(token)
	if err != nil {
		fmt.Fprintln(stderr, "runner-clock:", err)
		return 2
	}
	var hoursLimit *int
	if hoursProvided {
		hoursLimit = hours
	}
	code, err := scan(ctx, api, *repo, limit, hoursLimit, stdout)
	if err != nil {
		fmt.Fprintln(stderr, "runner-clock:", err)
		return 2
	}
	return code
}

func validRepo(repo string) bool {
	if !repoPattern.MatchString(repo) {
		return false
	}
	parts := strings.Split(repo, "/")
	return len(parts) == 2 && parts[0] != "." && parts[0] != ".." && parts[1] != "." && parts[1] != ".."
}

func ciRunLimit(raw string) (int, error) {
	if raw == "" {
		return ciRuns, nil
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value < 1 {
		return 0, errors.New("RA8_CLOCK_SCAN_RUNS must be a positive integer")
	}
	return value, nil
}

func loadToken(ctx context.Context) (string, error) {
	for _, name := range []string{"GH_TOKEN", "GITHUB_TOKEN"} {
		if value := strings.TrimSpace(os.Getenv(name)); value != "" {
			return value, nil
		}
	}
	executable, err := exec.LookPath("gh")
	if err == nil {
		command := exec.CommandContext(ctx, executable, "auth", "token")
		output, commandErr := command.Output()
		if commandErr == nil && strings.TrimSpace(string(output)) != "" {
			return strings.TrimSpace(string(output)), nil
		}
	}
	return "", errors.New("no GitHub API token; set GH_TOKEN or GITHUB_TOKEN, or authenticate with gh")
}

func newActionsAPI(token string) (*actionsAPI, error) {
	return newActionsAPIAt(apiBaseURL, token, nil)
}

func newActionsAPIAt(baseURL, token string, client *http.Client) (*actionsAPI, error) {
	token = strings.TrimSpace(token)
	if token == "" || strings.IndexFunc(token, unicode.IsSpace) >= 0 {
		return nil, errors.New("GitHub API token is empty or malformed")
	}
	base, err := url.Parse(baseURL)
	if err != nil || base == nil || base.Scheme != "https" || base.Host == "" || base.User != nil || base.RawQuery != "" || base.Fragment != "" {
		return nil, errors.New("GitHub Actions API requires an HTTPS origin")
	}
	if client == nil {
		client = &http.Client{Timeout: 30 * time.Second}
	}
	priorRedirect := client.CheckRedirect
	client.CheckRedirect = func(request *http.Request, via []*http.Request) error {
		if len(via) > 0 {
			previous := via[len(via)-1].URL
			if request.URL.Scheme != previous.Scheme || !strings.EqualFold(request.URL.Host, previous.Host) || request.URL.User != nil {
				return errors.New("refusing cross-origin GitHub API redirect")
			}
		}
		if priorRedirect != nil {
			return priorRedirect(request, via)
		}
		if len(via) >= 10 {
			return errors.New("too many GitHub API redirects")
		}
		return nil
	}
	return &actionsAPI{base: base, client: client, token: token}, nil
}

func (api *actionsAPI) get(ctx context.Context, endpoint string, query url.Values, target any) error {
	if api == nil || api.base == nil || api.client == nil {
		return errors.New("GitHub Actions API client is not configured")
	}
	if strings.ContainsAny(endpoint, "?#\\\r\n") {
		return errors.New("refusing an invalid GitHub API path")
	}
	for _, part := range strings.Split(endpoint, "/") {
		if part == "" || part == "." || part == ".." {
			return errors.New("refusing an invalid GitHub API path")
		}
	}
	requestURL := *api.base
	requestURL.Path = strings.TrimRight(requestURL.Path, "/") + "/" + endpoint
	requestURL.RawQuery = query.Encode()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, requestURL.String(), nil)
	if err != nil {
		return fmt.Errorf("build GitHub API request: %w", err)
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Authorization", "Bearer "+api.token)
	request.Header.Set("User-Agent", "ra8-firmware-runner-clock")
	request.Header.Set("X-GitHub-Api-Version", apiVersion)
	response, err := api.client.Do(request)
	if err != nil {
		return fmt.Errorf("GitHub API request failed: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("GitHub API GET failed: HTTP %d %s", response.StatusCode, http.StatusText(response.StatusCode))
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maxBody+1))
	if err != nil {
		return fmt.Errorf("read GitHub API response: %w", err)
	}
	if len(body) > maxBody {
		return fmt.Errorf("GitHub API response exceeds %d bytes", maxBody)
	}
	if err := json.Unmarshal(body, target); err != nil {
		return fmt.Errorf("decode GitHub API response: %w", err)
	}
	return nil
}

func parseTimestamp(value string) (time.Time, bool) {
	if value == "" {
		return time.Time{}, false
	}
	parsed, err := time.Parse(time.RFC3339Nano, value)
	if err != nil {
		return time.Time{}, false
	}
	return parsed, true
}

func scanJob(input job) []finding {
	var findings []finding
	var previousEnd time.Time
	var previousName string
	havePreviousEnd := false
	for _, item := range input.Steps {
		start, hasStart := parseTimestamp(item.StartedAt)
		end, hasEnd := parseTimestamp(item.CompletedAt)
		name := item.Name
		if name == "" {
			name = "?"
		}
		if hasStart && hasEnd && end.Before(start) {
			findings = append(findings, finding{
				kind: "finished-before-it-started", step: name,
				detail:  fmt.Sprintf("%s -> %s", start.UTC().Format(time.RFC3339), end.UTC().Format(time.RFC3339)),
				seconds: end.Sub(start).Seconds(), at: start,
			})
		}
		if hasStart && havePreviousEnd {
			gap := start.Sub(previousEnd)
			if gap < -overlapTolerance {
				findings = append(findings, finding{
					kind: "started-before-the-previous-step-finished", step: name,
					detail:  fmt.Sprintf("'%s' ended %s, '%s' began %s", previousName, previousEnd.UTC().Format(time.RFC3339), name, start.UTC().Format(time.RFC3339)),
					seconds: gap.Seconds(), at: start,
				})
			}
		}
		if hasEnd {
			previousEnd, previousName, havePreviousEnd = end, name, true
		}
	}
	return findings
}

func runs(ctx context.Context, api *actionsAPI, repo string, limit int, hours *int) ([]run, error) {
	perPage := limit
	if perPage > 100 {
		perPage = 100
	}
	query := url.Values{"status": {"completed"}, "per_page": {strconv.Itoa(perPage)}}
	var payload runList
	if err := api.get(ctx, "repos/"+repo+"/actions/runs", query, &payload); err != nil {
		return nil, err
	}
	if hours == nil {
		if len(payload.Runs) > limit {
			payload.Runs = payload.Runs[:limit]
		}
		return payload.Runs, nil
	}
	cutoff := time.Now().UTC().Add(-time.Duration(*hours) * time.Hour)
	filtered := make([]run, 0, len(payload.Runs))
	for _, item := range payload.Runs {
		started, ok := parseTimestamp(item.RunStarted)
		if !ok {
			started, ok = parseTimestamp(item.CreatedAt)
		}
		if ok && !started.Before(cutoff) {
			filtered = append(filtered, item)
		}
	}
	if len(filtered) > limit {
		filtered = filtered[:limit]
	}
	return filtered, nil
}

func scan(ctx context.Context, api *actionsAPI, repo string, limit int, hours *int, stdout io.Writer) (int, error) {
	recent, err := runs(ctx, api, repo, limit, hours)
	if err != nil {
		return 2, err
	}
	findings := make([]finding, 0)
	scannedRuns, scannedJobs, scannedSteps := 0, 0, 0
	for _, workflow := range recent {
		if err := ctx.Err(); err != nil {
			return 2, err
		}
		if workflow.ID <= 0 {
			return 2, fmt.Errorf("GitHub returned a workflow run without a valid id")
		}
		scannedRuns++
		var payload jobList
		endpoint := fmt.Sprintf("repos/%s/actions/runs/%d/jobs", repo, workflow.ID)
		if err := api.get(ctx, endpoint, url.Values{"per_page": {"100"}}, &payload); err != nil {
			return 2, err
		}
		for _, currentJob := range payload.Jobs {
			scannedJobs++
			scannedSteps += len(currentJob.Steps)
			for _, found := range scanJob(currentJob) {
				found.runner = currentJob.RunnerName
				if found.runner == "" {
					found.runner = "(unknown runner)"
				}
				found.job = currentJob.Name
				if found.job == "" {
					found.job = "?"
				}
				found.workflow = workflow.Name
				if found.workflow == "" {
					found.workflow = "?"
				}
				findings = append(findings, found)
			}
		}
	}
	if scannedSteps == 0 {
		return 2, errors.New("scan saw zero steps; widen --hours/--runs or check GitHub authentication")
	}
	report(stdout, findings, scannedRuns, scannedJobs, scannedSteps)
	if len(findings) != 0 {
		return 1, nil
	}
	return 0, nil
}

func report(stdout io.Writer, findings []finding, scannedRuns, scannedJobs, scannedSteps int) {
	fmt.Fprintf(stdout, "runner clock scan: %d completed runs, %d jobs, %d steps\n", scannedRuns, scannedJobs, scannedSteps)
	if len(findings) == 0 {
		fmt.Fprintln(stdout, "every step on every runner is time-ordered.")
		return
	}
	sort.Slice(findings, func(i, j int) bool { return findings[i].at.After(findings[j].at) })
	for _, item := range findings {
		fmt.Fprintf(stdout, "\nSKEW  runner=%s  %s / %s\n", item.runner, item.workflow, item.job)
		fmt.Fprintf(stdout, "      step '%s' %s\n", item.step, item.kind)
		fmt.Fprintf(stdout, "      %s  (%+.0fs)\n", item.detail, item.seconds)
	}
	perRunner := make(map[string]int)
	for _, item := range findings {
		perRunner[item.runner]++
	}
	runners := make([]string, 0, len(perRunner))
	for runner := range perRunner {
		runners = append(runners, runner)
	}
	sort.Slice(runners, func(i, j int) bool {
		if perRunner[runners[i]] == perRunner[runners[j]] {
			return runners[i] < runners[j]
		}
		return perRunner[runners[i]] > perRunner[runners[j]]
	})
	fmt.Fprintln(stdout, "\nper runner:")
	for _, runner := range runners {
		fmt.Fprintf(stdout, "  %s: %d skewed step(s)\n", runner, perRunner[runner])
	}
	fmt.Fprintf(stdout, "\nFAIL: %d step(s) on %d runner(s) are not time-ordered. Those runners moved their wall clock underneath a running job, so every time-budgeted gate that lands on them measures the wrong thing (#509). Fix the host's clock discipline -- slew, not step.\n", len(findings), len(perRunner))
}

func selfTest(stdout io.Writer) bool {
	cases := []struct {
		name string
		job  job
		want int
	}{
		{"a time-ordered job is clean", job{Steps: []step{{Name: "Set up job", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:00:04Z"}, {Name: "checkout", StartedAt: "2026-07-28T06:00:04Z", CompletedAt: "2026-07-28T06:00:09Z"}}}, 0},
		{"the observed win-ci-3 checkout (#509)", job{Steps: []step{{Name: "Set up job", StartedAt: "2026-07-28T06:38:16Z", CompletedAt: "2026-07-28T06:38:18Z"}, {Name: "checkout", StartedAt: "2026-07-28T06:38:18Z", CompletedAt: "2026-07-28T06:34:12Z"}}}, 1},
		{"a step that finished before it started", job{Steps: []step{{Name: "gate", StartedAt: "2026-07-28T06:23:40Z", CompletedAt: "2026-07-28T06:21:12Z"}}}, 1},
		{"a step that started before the previous finished", job{Steps: []step{{Name: "a", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:05:00Z"}, {Name: "b", StartedAt: "2026-07-28T06:01:00Z", CompletedAt: "2026-07-28T06:06:00Z"}}}, 1},
		{"one second of rounding is not a fault", job{Steps: []step{{Name: "a", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:00:05Z"}, {Name: "b", StartedAt: "2026-07-28T06:00:04Z", CompletedAt: "2026-07-28T06:00:09Z"}}}, 0},
		{"a skipped step has no timestamps to judge", job{Steps: []step{{Name: "never ran"}}}, 0},
		{"a job with no steps at all", job{}, 0},
	}
	fmt.Fprintln(stdout, "ra8ci runner-clock selftest:")
	passed := true
	for _, item := range cases {
		got := len(scanJob(item.job))
		ok := got == item.want
		if !ok {
			passed = false
		}
		fmt.Fprintf(stdout, "  %s %s: expected %d, got %d\n", map[bool]string{true: "ok ", false: "FAIL"}[ok], item.name, item.want, got)
	}
	if !passed {
		fmt.Fprintln(stdout, "SELFTEST FAILED: the clock detector no longer behaves as documented.")
		return false
	}
	fmt.Fprintf(stdout, "  %d/%d cases as documented.\n", len(cases), len(cases))
	return true
}
