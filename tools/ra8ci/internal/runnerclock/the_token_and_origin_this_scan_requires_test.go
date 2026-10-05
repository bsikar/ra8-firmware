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
	"strings"
	"testing"
	"time"
)

// A scan cannot begin without a token and an origin, and both of those
// decisions are taken before a single request is spent. These tests hold the
// token search (environment first, then the GitHub CLI) and the origin the
// client will accept, so a misconfigured box is told what is wrong instead of
// reaching the network with a credential it should not have used.

// noCLI puts an empty directory on PATH so Run's missing-token behavior stays
// independent of any GitHub CLI installed on the host.
func noCLI(t *testing.T) {
	t.Helper()
	t.Setenv("PATH", t.TempDir())
}

func silentEnvironment(t *testing.T) {
	t.Helper()
	t.Setenv("GH_TOKEN", "")
	t.Setenv("GITHUB_TOKEN", "")
}

func TestTheEnvironmentTokenIsPreferredOverTheCLI(t *testing.T) {
	cliCalled := false
	lookup := func(name string) string {
		if name == "GH_TOKEN" {
			return "env-token"
		}
		return "second-token"
	}
	token, err := loadTokenWith(context.Background(), lookup, func(context.Context) (string, error) {
		cliCalled = true
		return "cli-token", nil
	})
	if err != nil || token != "env-token" {
		t.Fatalf("loadTokenWith = %q, %v", token, err)
	}
	if cliCalled {
		t.Fatal("CLI token source was called despite an environment token")
	}
}

func TestTheSecondEnvironmentNameIsConsultedWhenTheFirstIsBlank(t *testing.T) {
	for _, first := range []string{"", "   ", "\t\n"} {
		token, err := loadTokenWith(context.Background(), func(name string) string {
			if name == "GH_TOKEN" {
				return first
			}
			return "  second-token\n"
		}, nil)
		if err != nil || token != "second-token" {
			t.Fatalf("GH_TOKEN=%q: loadTokenWith = %q, %v", first, token, err)
		}
	}
}

func TestTheTokenIsAskedOfTheCLIWhenTheEnvironmentIsSilent(t *testing.T) {
	cliCalled := false
	token, err := loadTokenWith(context.Background(), func(string) string { return " " }, func(context.Context) (string, error) {
		cliCalled = true
		return "  cli-token  ", nil
	})
	if err != nil || token != "cli-token" {
		t.Fatalf("loadTokenWith = %q, %v", token, err)
	}
	if !cliCalled {
		t.Fatal("CLI token source was not called after empty environment values")
	}
}

func TestACLIThatAnswersNothingIsNotATokenSource(t *testing.T) {
	for _, answer := range []struct {
		name  string
		token string
		err   error
	}{{"empty", "", nil}, {"blank", "   ", nil}, {"failed", "", errors.New("fake source failed")}} {
		t.Run(answer.name, func(t *testing.T) {
			token, err := loadTokenWith(context.Background(), func(string) string { return "" }, func(context.Context) (string, error) {
				return answer.token, answer.err
			})
			if err == nil || token != "" || !strings.Contains(err.Error(), "GH_TOKEN") {
				t.Fatalf("loadTokenWith = %q, %v", token, err)
			}
		})
	}
}

func TestNoEnvironmentAndNoCLIIsRefusedByName(t *testing.T) {
	token, err := loadTokenWith(context.Background(), func(string) string { return "" }, nil)
	if err == nil || token != "" {
		t.Fatalf("loadToken = %q, %v", token, err)
	}
	for _, want := range []string{"no GitHub API token", "GH_TOKEN", "GITHUB_TOKEN", "gh"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q omits %q", err.Error(), want)
		}
	}
}

func TestACancelledContextIsPassedToTheTokenSource(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	called := false
	token, err := loadTokenWith(ctx, func(string) string { return "" }, func(ctx context.Context) (string, error) {
		called = true
		return "", ctx.Err()
	})
	if err == nil || token != "" {
		t.Fatalf("cancelled loadTokenWith = %q, %v", token, err)
	}
	if !called {
		t.Fatal("token source was not called")
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
