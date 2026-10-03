// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

// An installation token is what lets this plane act as the App, so what it
// is minted from, where it is minted, and the client it is minted over are
// all decided before a request is sent. Nothing here reaches GitHub.

// mintedAppKey builds a real RSA key and its PEM, once per test that needs one.
func mintedAppKey(t *testing.T) (*rsa.PrivateKey, []byte) {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	body := pem.EncodeToMemory(&pem.Block{
		Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key),
	})
	return key, body
}

// The App API origin is exactly the public one. Anything else would send an
// App JWT, which is a credential for the whole installation, somewhere GitHub
// does not answer from.
func TestTheAppAPIOriginIsExactlyThePublicOne(t *testing.T) {
	for _, base := range []string{"", "https://api.github.com", "https://API.GitHub.COM", "https://api.github.com:443"} {
		origin, err := validAppAPIOrigin(base)
		if err != nil || origin == nil {
			t.Fatalf("%q was refused: %v", base, err)
		}
	}
	for name, base := range map[string]string{
		"plain HTTP":     "http://api.github.com",
		"another host":   "https://github.example.com",
		"the web host":   "https://github.com",
		"a path":         "https://api.github.com/v3",
		"a query":        "https://api.github.com?x=1",
		"a fragment":     "https://api.github.com#f",
		"userinfo":       "https://user:pass@api.github.com",
		"another port":   "https://api.github.com:8443",
		"trailing space": "https://api.github.com ",
		"unparseable":    "https://api.github.com/%zz",
	} {
		if _, err := validAppAPIOrigin(base); err == nil {
			t.Fatalf("%s (%q) was accepted", name, base)
		}
	}
}

// The client this plane talks to GitHub with returns redirects rather than
// following them, so a redirected request never carries an installation
// token to another origin, and it never inherits a proxy from the
// environment.
func TestTheAppClientNeverFollowsARedirectOrInheritsAProxy(t *testing.T) {
	built := appHTTPClient(nil)
	if built == nil || built.Timeout != 10*time.Second {
		t.Fatalf("a built client = %+v", built)
	}
	if err := built.CheckRedirect(nil, nil); err != http.ErrUseLastResponse {
		t.Fatalf("a built client follows redirects: %v", err)
	}
	if transport, ok := built.Transport.(*http.Transport); !ok || transport.Proxy != nil {
		t.Fatalf("a built client carries a proxy: %+v", built.Transport)
	}

	// A seam is COPIED, never mutated: the caller's own client keeps
	// following redirects and keeps whatever transport it was given.
	proxied := &http.Transport{Proxy: http.ProxyFromEnvironment}
	seam := &http.Client{Transport: proxied}
	copied := appHTTPClient(seam)
	if copied == seam {
		t.Fatal("the seam itself was handed back")
	}
	if seam.CheckRedirect != nil || seam.Transport != proxied || proxied.Proxy == nil {
		t.Fatal("the caller's own client was mutated")
	}
	if transport, ok := copied.Transport.(*http.Transport); !ok || transport.Proxy != nil {
		t.Fatalf("the copy kept the proxy: %+v", copied.Transport)
	}

	// A seam with no transport of its own is given the same proxy-less one.
	bare := appHTTPClient(&http.Client{})
	if transport, ok := bare.Transport.(*http.Transport); !ok || transport.Proxy != nil {
		t.Fatalf("a bare seam = %+v", bare.Transport)
	}
	if err := bare.CheckRedirect(nil, nil); err != http.ErrUseLastResponse {
		t.Fatalf("a bare seam follows redirects: %v", err)
	}
}

// minting stands an endpoint up and hands back an installation bound to it,
// along with a count of how many times it was asked.
func minting(t *testing.T, handler func(w http.ResponseWriter, r *http.Request)) (*appInstallation, *int) {
	t.Helper()
	key, _ := mintedAppKey(t)
	asked := 0
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		asked++
		handler(w, r)
	}))
	t.Cleanup(server.Close)
	base, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	return &appInstallation{
		apiURL: base, key: key, client: server.Client(), clientID: "Iv1.0123456789abcdef",
		installationID: 4242, repository: "ra8-firmware",
		permissions: map[string]string{"actions": "read"},
	}, &asked
}

// A token is minted once and reused until it is nearly spent, because every
// mint costs a request the plane does not have to make.
func TestAnInstallationTokenIsMintedOnceAndReused(t *testing.T) {
	installation, asked := minting(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost ||
			r.URL.Path != "/app/installations/4242/access_tokens" {
			t.Errorf("request = %s %s", r.Method, r.URL.Path)
		}
		if !strings.HasPrefix(r.Header.Get("Authorization"), "Bearer ") ||
			r.Header.Get("X-GitHub-Api-Version") != githubAPIVersion {
			t.Error("the App JWT or the API version was not sent")
		}
		var body installationTokenRequest
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			t.Error(err)
		}
		if len(body.Repositories) != 1 || body.Repositories[0] != "ra8-firmware" ||
			body.Permissions["actions"] != "read" {
			t.Errorf("the token was asked for %+v", body)
		}
		w.WriteHeader(http.StatusCreated)
		_ = json.NewEncoder(w).Encode(installationTokenResponse{
			Token: "ghs-minted", ExpiresAt: time.Now().Add(time.Hour),
		})
	})

	first, err := installation.accessToken(context.Background())
	if err != nil || first != "ghs-minted" {
		t.Fatalf("mint = %q, %v", first, err)
	}
	again, err := installation.accessToken(context.Background())
	if err != nil || again != first {
		t.Fatalf("second ask = %q, %v", again, err)
	}
	if *asked != 1 {
		t.Fatalf("the endpoint was asked %d times for one live token", *asked)
	}

	// A token inside the last minute of its life is spent, not reused.
	installation.tokenTill = time.Now().Add(30 * time.Second)
	if _, err := installation.accessToken(context.Background()); err != nil {
		t.Fatal(err)
	}
	if *asked != 2 {
		t.Fatalf("a nearly-spent token was reused (%d asks)", *asked)
	}
}

// A token the plane cannot rely on is refused rather than cached, since a
// cached bad token would fail every request until it expired.
func TestATokenThePlaneCannotRelyOnIsNotCached(t *testing.T) {
	for name, answer := range map[string]func(w http.ResponseWriter, r *http.Request){
		"refused": func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusForbidden)
		},
		"no token": func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{ExpiresAt: time.Now().Add(time.Hour)})
		},
		"already expiring": func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{
				Token: "ghs-stale", ExpiresAt: time.Now().Add(10 * time.Second),
			})
		},
		"unreadable": func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_, _ = w.Write([]byte("{"))
		},
	} {
		installation, _ := minting(t, answer)
		if _, err := installation.accessToken(context.Background()); err == nil {
			t.Fatalf("%s was accepted", name)
		}
		if installation.token != "" {
			t.Fatalf("%s was cached as %q", name, installation.token)
		}
	}
}
