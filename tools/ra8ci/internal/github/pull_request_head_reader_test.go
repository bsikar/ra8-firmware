// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const pullHeadSHA = "0123456789abcdef0123456789abcdef01234567"

// pullRequestBody renders one pull request document the way GitHub sends it.
// headRepo empty means the head repository is absent, which is what a deleted
// fork looks like.
func pullRequestBody(number int, state string, merged bool, sha, baseRef, headRepo string) string {
	document := map[string]any{
		"number": number,
		"state":  state,
		"merged": merged,
		"head":   map[string]any{"sha": sha},
		"base": map[string]any{
			"ref":  baseRef,
			"repo": map[string]any{"full_name": "bsikar/ra8-firmware"},
		},
	}
	if headRepo != "" {
		document["head"] = map[string]any{"sha": sha, "repo": map[string]any{"full_name": headRepo}}
	}
	encoded, err := json.Marshal(document)
	if err != nil {
		panic(err)
	}
	return string(encoded)
}

// newPullRequestHeadReader builds a reader pointed at a test server that
// answers the installation-token mint and then whatever handler is given. It
// follows the shape the other readers' tests use: a TLS server, the App key on
// disk, and the transport rewrite that lets the reader keep its real origin
// check.
func newPullRequestHeadReader(t *testing.T, handler http.HandlerFunc) *PullRequestHeadReader {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
		t.Fatalf("write key: %v", err)
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/access_tokens") {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "installation-token", ExpiresAt: time.Now().Add(time.Hour)})
			return
		}
		handler(w, r)
	}))
	t.Cleanup(server.Close)
	transport := server.Client().Transport.(*http.Transport).Clone()
	transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec // deterministic test server
	client := &http.Client{Transport: checkRunTestTransport{inner: transport, host: strings.TrimPrefix(server.URL, "https://")}}
	reader, err := NewPullRequestHeadReader(PullRequestHeadReaderConfig{
		AppClientID: "Iv1.pullrequesthead", InstallationID: 42, PrivateKeyFile: keyPath,
		Owner: "bsikar", Repository: "ra8-firmware", httpClient: client,
	})
	if err != nil {
		t.Fatalf("build reader: %v", err)
	}
	return reader
}

// pullRequestHeadKey writes an App key for a configuration test that never
// reaches GitHub.
func pullRequestHeadKey(t *testing.T) string {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	keyPath := filepath.Join(t.TempDir(), "app.pem")
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
		t.Fatalf("write key: %v", err)
	}
	return keyPath
}

func TestTheHeadOfAMergedPullRequestIsReported(t *testing.T) {
	reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, pullRequestBody(1481, "closed", true, pullHeadSHA, "ra8ci/dev", "bsikar/ra8-firmware"))
	})
	head, err := reader.Head(context.Background(), 1481)
	if err != nil {
		t.Fatalf("read head: %v", err)
	}
	if head.Number != 1481 || head.HeadSHA != pullHeadSHA {
		t.Fatalf("pull request %d at %q, want 1481 at %q", head.Number, head.HeadSHA, pullHeadSHA)
	}
	if head.BaseRef != "ra8ci/dev" || head.State != "closed" || !head.Merged {
		t.Fatalf("base %q state %q merged %v, want ra8ci/dev closed true", head.BaseRef, head.State, head.Merged)
	}
	if head.FromFork {
		t.Fatal("a head in this repository was reported as a fork")
	}
	if head.HeadRepository != "bsikar/ra8-firmware" {
		t.Fatalf("head repository %q, want bsikar/ra8-firmware", head.HeadRepository)
	}
}

// An open pull request is answered as readily as a merged one: which pull
// requests are representative is the operator's judgement, stated when they
// choose the numbers.
func TestAnOpenPullRequestIsAnsweredToo(t *testing.T) {
	reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, pullRequestBody(1500, "open", false, pullHeadSHA, "main", "bsikar/ra8-firmware"))
	})
	head, err := reader.Head(context.Background(), 1500)
	if err != nil {
		t.Fatalf("read head: %v", err)
	}
	if head.State != "open" || head.Merged {
		t.Fatalf("state %q merged %v, want open false", head.State, head.Merged)
	}
	if head.HeadSHA != pullHeadSHA {
		t.Fatalf("head %q, want %q", head.HeadSHA, pullHeadSHA)
	}
}

// The fork is reported rather than refused. Evidence gathered on a fork pull
// request is evidence about code this repository did not control, and an
// operator moving a merge gate has to see which side of that line it came
// from.
func TestAForkHeadIsReportedNotRefused(t *testing.T) {
	reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, pullRequestBody(1501, "open", false, pullHeadSHA, "main", "someone-else/ra8-firmware"))
	})
	head, err := reader.Head(context.Background(), 1501)
	if err != nil {
		t.Fatalf("read head: %v", err)
	}
	if !head.FromFork {
		t.Fatal("a head in another repository was not reported as a fork")
	}
	if head.HeadRepository != "someone-else/ra8-firmware" {
		t.Fatalf("head repository %q, want someone-else/ra8-firmware", head.HeadRepository)
	}
	if head.HeadSHA != pullHeadSHA {
		t.Fatalf("head %q, want %q", head.HeadSHA, pullHeadSHA)
	}
}

// A head repository GitHub did not send is a fork that has since been deleted.
// Reading the absence as "same repository" would call the one case nobody can
// go and look at the trusted one.
func TestAMissingHeadRepositoryIsAFork(t *testing.T) {
	reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, pullRequestBody(1502, "closed", false, pullHeadSHA, "main", ""))
	})
	head, err := reader.Head(context.Background(), 1502)
	if err != nil {
		t.Fatalf("read head: %v", err)
	}
	if !head.FromFork {
		t.Fatal("an absent head repository was not reported as a fork")
	}
	if head.HeadRepository != "" {
		t.Fatalf("head repository %q, want empty", head.HeadRepository)
	}
}

// An unreadable pull request is never a pull request with no head: an empty
// head would send every downstream read at the zero commit.
func TestAnUnreadablePullRequestIsNeverAnEmptyHead(t *testing.T) {
	for _, status := range []int{
		http.StatusNotFound,
		http.StatusForbidden,
		http.StatusUnauthorized,
		http.StatusInternalServerError,
	} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(status)
			})
			head, err := reader.Head(context.Background(), 1481)
			if !errors.Is(err, ErrPullRequestUnreadable) {
				t.Fatalf("error %v, want ErrPullRequestUnreadable", err)
			}
			if head != (PullRequestHead{}) {
				t.Fatalf("a refused read reported %+v", head)
			}
		})
	}
}

func TestAPullRequestThatDoesNotDescribeItselfIsRefused(t *testing.T) {
	for name, body := range map[string]string{
		"another number":     pullRequestBody(9, "open", false, pullHeadSHA, "main", "bsikar/ra8-firmware"),
		"another repository": strings.Replace(pullRequestBody(1481, "open", false, pullHeadSHA, "main", "bsikar/ra8-firmware"), `"full_name":"bsikar/ra8-firmware"`, `"full_name":"someone-else/other"`, 1),
		"no state":           pullRequestBody(1481, "", false, pullHeadSHA, "main", "bsikar/ra8-firmware"),
		"not a document":     "not json",
	} {
		t.Run(name, func(t *testing.T) {
			reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
				fmt.Fprint(w, body)
			})
			head, err := reader.Head(context.Background(), 1481)
			if !errors.Is(err, ErrPullRequestUnreadable) {
				t.Fatalf("error %v, want ErrPullRequestUnreadable", err)
			}
			if head != (PullRequestHead{}) {
				t.Fatalf("a refused read reported %+v", head)
			}
		})
	}
}

// A head that is not a commit is its own answer, separate from a read that did
// not happen: GitHub answered, and what it named cannot be read about.
func TestAHeadThatIsNotACommitIsItsOwnRefusal(t *testing.T) {
	for name, sha := range map[string]string{
		"empty":    "",
		"short":    "0123456",
		"not hex":  strings.Repeat("z", 40),
		"a ref":    "refs/heads/main",
		"too long": strings.Repeat("a", 41),
		"with pad": " " + pullHeadSHA,
	} {
		t.Run(name, func(t *testing.T) {
			reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
				fmt.Fprint(w, pullRequestBody(1481, "open", false, sha, "main", "bsikar/ra8-firmware"))
			})
			head, err := reader.Head(context.Background(), 1481)
			if !errors.Is(err, ErrPullRequestHeadUnknown) {
				t.Fatalf("error %v, want ErrPullRequestHeadUnknown", err)
			}
			if head != (PullRequestHead{}) {
				t.Fatalf("a refused read reported %+v", head)
			}
		})
	}
}

// A number that cannot name a pull request is refused before a token is
// minted, so a bad ask costs GitHub nothing.
func TestANumberThatIsNotAPullRequestMintsNoToken(t *testing.T) {
	asked := false
	reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
		asked = true
	})
	for _, number := range []int{0, -1, -1481} {
		if _, err := reader.Head(context.Background(), number); err == nil {
			t.Fatalf("pull request %d was accepted", number)
		}
	}
	if asked {
		t.Fatal("a refused number reached GitHub")
	}
}

// Reading which commit a pull request is at cannot change it.
func TestReadingTheHeadOnlyReads(t *testing.T) {
	reads := 0
	methods := []string{}
	reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {
		reads++
		methods = append(methods, r.Method)
		if !strings.HasSuffix(r.URL.Path, "/repos/bsikar/ra8-firmware/pulls/1481") {
			t.Errorf("read %q", r.URL.Path)
		}
		fmt.Fprint(w, pullRequestBody(1481, "closed", true, pullHeadSHA, "ra8ci/dev", "bsikar/ra8-firmware"))
	})
	if _, err := reader.Head(context.Background(), 1481); err != nil {
		t.Fatalf("read head: %v", err)
	}
	if reads != 1 {
		t.Fatalf("the pull request was read %d times, want once", reads)
	}
	for _, method := range methods {
		if method != http.MethodGet {
			t.Fatalf("the reader used %s", method)
		}
	}
}

// The token holds pull_requests:read and nothing else. Listing the runs on the
// head this reports is actions:read and belongs beside the reader that already
// holds it.
func TestThePullRequestTokenReadsPullRequestsAlone(t *testing.T) {
	reader := newPullRequestHeadReader(t, func(w http.ResponseWriter, r *http.Request) {})
	permissions := reader.tokens.permissions
	if len(permissions) != 1 || permissions["pull_requests"] != "read" {
		t.Fatalf("token permissions %v, want exactly pull_requests:read", permissions)
	}
	if reader.tokens.repository != "ra8-firmware" {
		t.Fatalf("token repository %q, want ra8-firmware", reader.tokens.repository)
	}
}

func TestAPullRequestHeadReaderRefusesAnInvalidConfiguration(t *testing.T) {
	base := PullRequestHeadReaderConfig{
		APIBaseURL:     "https://api.github.com",
		AppClientID:    "Iv1.pullrequesthead",
		InstallationID: 42,
		PrivateKeyFile: pullRequestHeadKey(t),
		Owner:          "bsikar",
		Repository:     "ra8-firmware",
	}
	for name, mangle := range map[string]func(*PullRequestHeadReaderConfig){
		"no client id":    func(c *PullRequestHeadReaderConfig) { c.AppClientID = "" },
		"no installation": func(c *PullRequestHeadReaderConfig) { c.InstallationID = 0 },
		"no key file":     func(c *PullRequestHeadReaderConfig) { c.PrivateKeyFile = "" },
		"no owner":        func(c *PullRequestHeadReaderConfig) { c.Owner = "" },
		"no repository":   func(c *PullRequestHeadReaderConfig) { c.Repository = "" },
		"bad origin":      func(c *PullRequestHeadReaderConfig) { c.APIBaseURL = "http://example.invalid" },
	} {
		t.Run(name, func(t *testing.T) {
			config := base
			mangle(&config)
			if _, err := NewPullRequestHeadReader(config); err == nil {
				t.Fatal("an invalid configuration built a reader")
			}
		})
	}
}
