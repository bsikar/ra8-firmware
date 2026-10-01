// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// A scan cannot begin without a token and an origin, and both of those
// decisions are taken before a single request is spent. These tests hold the
// token search (environment first, then the GitHub CLI) and the origin the
// client will accept, so a misconfigured box is told what is wrong instead of
// reaching the network with a credential it should not have used.

// noCLI puts an empty directory on PATH so the GitHub CLI cannot be found,
// which is the only way to reach loadToken's refusal on a box where gh exists.
func noCLI(t *testing.T) {
	t.Helper()
	t.Setenv("PATH", t.TempDir())
}

// plantCLI puts a stand-in gh on PATH whose `auth token` answer is the script
// body handed in, so the CLI branch is exercised without a real login.
func plantCLI(t *testing.T, body string) {
	t.Helper()
	home := t.TempDir()
	script := filepath.Join(home, "gh")
	if err := os.WriteFile(script, []byte("#!/bin/sh\n"+body+"\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", home)
}

func silentEnvironment(t *testing.T) {
	t.Helper()
	t.Setenv("GH_TOKEN", "")
	t.Setenv("GITHUB_TOKEN", "")
}

func TestTheEnvironmentTokenIsPreferredOverTheCLI(t *testing.T) {
	plantCLI(t, "echo cli-token")
	t.Setenv("GH_TOKEN", "env-token")
	t.Setenv("GITHUB_TOKEN", "second-token")
	token, err := loadToken(context.Background())
	if err != nil || token != "env-token" {
		t.Fatalf("loadToken = %q, %v", token, err)
	}
}

func TestTheSecondEnvironmentNameIsConsultedWhenTheFirstIsBlank(t *testing.T) {
	noCLI(t)
	for _, first := range []string{"", "   ", "\t\n"} {
		t.Setenv("GH_TOKEN", first)
		t.Setenv("GITHUB_TOKEN", "  second-token\n")
		token, err := loadToken(context.Background())
		if err != nil || token != "second-token" {
			t.Fatalf("GH_TOKEN=%q: loadToken = %q, %v", first, token, err)
		}
	}
}

func TestTheTokenIsAskedOfTheCLIWhenTheEnvironmentIsSilent(t *testing.T) {
	silentEnvironment(t)
	plantCLI(t, "echo '  cli-token  '")
	token, err := loadToken(context.Background())
	if err != nil || token != "cli-token" {
		t.Fatalf("loadToken = %q, %v", token, err)
	}
}

func TestACLIThatAnswersNothingIsNotATokenSource(t *testing.T) {
	silentEnvironment(t)
	for _, body := range []string{"echo ''", "printf '   '", "exit 1", "echo oops 1>&2; exit 4"} {
		plantCLI(t, body)
		token, err := loadToken(context.Background())
		if err == nil || token != "" || !strings.Contains(err.Error(), "GH_TOKEN") {
			t.Fatalf("gh %q: loadToken = %q, %v", body, token, err)
		}
	}
}

func TestNoEnvironmentAndNoCLIIsRefusedByName(t *testing.T) {
	silentEnvironment(t)
	noCLI(t)
	token, err := loadToken(context.Background())
	if err == nil || token != "" {
		t.Fatalf("loadToken = %q, %v", token, err)
	}
	for _, want := range []string{"no GitHub API token", "GH_TOKEN", "GITHUB_TOKEN", "gh"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q omits %q", err.Error(), want)
		}
	}
}

func TestACancelledScanAsksTheCLIForNothing(t *testing.T) {
	silentEnvironment(t)
	plantCLI(t, "echo cli-token")
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	token, err := loadToken(ctx)
	if err == nil || token != "" {
		t.Fatalf("cancelled loadToken = %q, %v", token, err)
	}
}

func TestTheDefaultOriginIsTheGitHubAPI(t *testing.T) {
	api, err := newActionsAPI("  good-token  ")
	if err != nil {
		t.Fatal(err)
	}
	if api.base == nil || api.base.String() != apiBaseURL {
		t.Fatalf("base = %v", api.base)
	}
	if api.token != "good-token" {
		t.Fatalf("token = %q", api.token)
	}
	if api.client == nil || api.client.Timeout != 30*time.Second {
		t.Fatalf("client = %+v", api.client)
	}
	if api.client.CheckRedirect == nil {
		t.Fatal("the default client carries no redirect policy")
	}
}

func TestAMalformedTokenIsRefusedBeforeAnyRequest(t *testing.T) {
	for _, token := range []string{"", "    ", "\t", "two words", "line\nbreak", "tab\there"} {
		api, err := newActionsAPI(token)
		if err == nil || api != nil || !strings.Contains(err.Error(), "empty or malformed") {
			t.Fatalf("token %q = %v, %v", token, api, err)
		}
	}
}

func TestAnOriginThatIsNotAPlainHTTPSHostIsRefused(t *testing.T) {
	for _, base := range []string{
		"",
		"http://api.github.com",
		"https://",
		"ftp://api.github.com",
		"https://user:pass@api.github.com",
		"https://api.github.com?token=leaked",
		"https://api.github.com#fragment",
		"https://api.github.com/%zz",
		"api.github.com",
	} {
		api, err := newActionsAPIAt(base, "good-token", nil)
		if err == nil || api != nil || !strings.Contains(err.Error(), "HTTPS origin") {
			t.Fatalf("base %q = %v, %v", base, api, err)
		}
	}
}

// servedTwice answers a same-origin redirect once and then a JSON body, so the
// redirect policy is judged on a hop it should allow rather than only on the
// cross-origin hop the package already refuses.
func servedTwice(t *testing.T) *httptest.Server {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/moved") {
			w.Write([]byte(`{"workflow_runs":[]}`))
			return
		}
		http.Redirect(w, r, "/moved", http.StatusFound)
	}))
	t.Cleanup(server.Close)
	return server
}

func TestASameOriginRedirectIsFollowed(t *testing.T) {
	server := servedTwice(t)
	api, err := newActionsAPIAt(server.URL, "good-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	var payload runList
	if err := api.get(context.Background(), "repos/owner/repo/actions/runs", url.Values{}, &payload); err != nil {
		t.Fatalf("same-origin redirect refused: %v", err)
	}
}

func TestAPriorRedirectPolicyIsStillHonoured(t *testing.T) {
	server := servedTwice(t)
	client := server.Client()
	client.CheckRedirect = func(*http.Request, []*http.Request) error {
		return errors.New("the caller's own policy refused this hop")
	}
	api, err := newActionsAPIAt(server.URL, "good-token", client)
	if err != nil {
		t.Fatal(err)
	}
	var payload runList
	err = api.get(context.Background(), "repos/owner/repo/actions/runs", url.Values{}, &payload)
	if err == nil || !strings.Contains(err.Error(), "the caller's own policy refused this hop") {
		t.Fatalf("prior policy result = %v", err)
	}
}

func TestARedirectChainIsBoundedRatherThanWalkedForever(t *testing.T) {
	hops := 0
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hops++
		http.Redirect(w, r, "/again", http.StatusFound)
	}))
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "good-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	var payload runList
	err = api.get(context.Background(), "repos/owner/repo/actions/runs", url.Values{}, &payload)
	if err == nil || !strings.Contains(err.Error(), "too many GitHub API redirects") {
		t.Fatalf("redirect loop result = %v after %d hops", err, hops)
	}
	if hops > 11 {
		t.Fatalf("the loop was walked %d times", hops)
	}
}

// ran is one invocation of Run: its exit code and both streams.
type ran struct {
	code   int
	stdout string
	stderr string
}

func runClock(t *testing.T, args ...string) ran {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), args, &stdout, &stderr)
	return ran{code: code, stdout: stdout.String(), stderr: stderr.String()}
}

func TestAScanWithNoTokenAnywhereIsRefusedBeforeTheNetwork(t *testing.T) {
	silentEnvironment(t)
	noCLI(t)
	got := runClock(t, "--runs", "1")
	if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "no GitHub API token") {
		t.Fatalf("no-token scan = %+v", got)
	}
}

func TestAScanWithAMalformedEnvironmentTokenIsRefusedBeforeTheNetwork(t *testing.T) {
	noCLI(t)
	t.Setenv("GH_TOKEN", "two words")
	got := runClock(t, "--runs", "1")
	if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "empty or malformed") {
		t.Fatalf("malformed-token scan = %+v", got)
	}
}

func TestTheSelfTestCannotBeCombinedWithScanOptions(t *testing.T) {
	for _, args := range [][]string{
		{"--selftest", "--runs", "5"},
		{"--selftest", "--repo", "owner/other"},
		{"--selftest", "--hours", "1"},
		{"--selftest", "--ci-scan"},
	} {
		got := runClock(t, args...)
		if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "--selftest cannot be combined") {
			t.Fatalf("%q = %+v", args, got)
		}
	}
}

// --hours 0 is the default, so naming it explicitly must still count as a scan
// option: the flag is judged on whether it was VISITED, not on its value.
func TestTheSelfTestRefusesAnExplicitDefaultHoursWindow(t *testing.T) {
	got := runClock(t, "--selftest", "--hours", "0")
	if got.code != 2 || !strings.Contains(got.stderr, "--selftest cannot be combined") {
		t.Fatalf("explicit default hours = %+v", got)
	}
}

func TestACIScanTakesItsRunCountFromTheEnvironment(t *testing.T) {
	silentEnvironment(t)
	noCLI(t)
	for _, raw := range []string{"abc", "0", "-3", "1.5"} {
		t.Setenv("RA8_CLOCK_SCAN_RUNS", raw)
		got := runClock(t, "--ci-scan")
		if got.code != 2 || !strings.Contains(got.stderr, "RA8_CLOCK_SCAN_RUNS must be a positive integer") {
			t.Fatalf("RA8_CLOCK_SCAN_RUNS=%q = %+v", raw, got)
		}
	}
	t.Setenv("RA8_CLOCK_SCAN_RUNS", "5")
	got := runClock(t, "--ci-scan")
	if got.code != 2 || !strings.Contains(got.stderr, "no GitHub API token") {
		t.Fatalf("an accepted run count should carry on to the token: %+v", got)
	}
}

func TestANegativeHoursWindowIsRefused(t *testing.T) {
	got := runClock(t, "--hours", "-1")
	if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "--hours cannot be negative") {
		t.Fatalf("negative hours = %+v", got)
	}
}

func TestAnUnreadableInvocationIsRefusedWithTheUsageLine(t *testing.T) {
	for _, args := range [][]string{
		{"--unknown"},
		{"stray-argument"},
		{"--runs", "not-a-number"},
		{"--selftest", "stray"},
	} {
		got := runClock(t, args...)
		if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "usage: ra8ci runner-clock") {
			t.Fatalf("%q = %+v", args, got)
		}
	}
}

func TestRunWithoutTheWritersItWasPromised(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(nil, []string{"--selftest"}, &stdout, &stderr); code != 2 {
		t.Fatalf("no context = %d", code)
	}
	if !strings.Contains(stderr.String(), "invalid input") || stdout.String() != "" {
		t.Fatalf("no context stderr = %q stdout = %q", stderr.String(), stdout.String())
	}
	var absent io.Writer
	if code := Run(context.Background(), []string{"--selftest"}, absent, &stderr); code != 2 {
		t.Fatalf("no stdout = %d", code)
	}
	if code := Run(context.Background(), []string{"--selftest"}, &stdout, absent); code != 2 {
		t.Fatalf("no stderr = %d", code)
	}
}
