// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strings"
	"testing"
	"time"
)

// An installation token is the plane's whole authority at the forge, scoped to
// one repository and one permission set. A mint that answered a token the
// endpoint never really granted, or that kept re-minting a sound one, would
// either send an empty bearer at the Actions API or spend the App's rate limit
// on tokens it already holds.

// mintingInstallation builds an installation whose token requests land on one
// handler, so the handler can answer the mint any way it likes. The fixtures
// in metadata_test.go and the reader harnesses all mint a sound token inline
// and cannot answer a bad one.
func mintingInstallation(t *testing.T, handler http.HandlerFunc) *appInstallation {
	t.Helper()
	key, err := loadAppPrivateKey(soundKeyFile(t))
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewTLSServer(handler)
	t.Cleanup(server.Close)
	destination, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	apiURL, err := validAppAPIOrigin("https://api.github.com")
	if err != nil {
		t.Fatal(err)
	}
	client := server.Client()
	client.Transport = metadataTestTransport{destination: destination, transport: client.Transport}
	return &appInstallation{
		apiURL: apiURL, key: key, client: client, clientID: "client-id", installationID: 42,
		repository: "ra8-firmware", permissions: map[string]string{"actions": "read"},
	}
}

// granting answers every token request with one token and the given lifetime,
// and counts the mints.
func granting(token string, lifetime time.Duration, mints *int) http.HandlerFunc {
	return func(w http.ResponseWriter, _ *http.Request) {
		*mints++
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusCreated)
		_ = json.NewEncoder(w).Encode(installationTokenResponse{
			Token: token, ExpiresAt: time.Now().Add(lifetime),
		})
	}
}

// A token with life left in it is reused, and one inside the last minute of
// its life is replaced. The minute of margin is what stops a token expiring
// in flight between the mint and the read it authorizes.
func TestATokenIsMintedOnceAndReplacedOnlyNearItsExpiry(t *testing.T) {
	mints := 0
	installation := mintingInstallation(t, granting("long-lived", time.Hour, &mints))
	first, err := installation.accessToken(context.Background())
	if err != nil || first != "long-lived" {
		t.Fatalf("first mint answered %q, %v", first, err)
	}
	second, err := installation.accessToken(context.Background())
	if err != nil || second != "long-lived" {
		t.Fatalf("second ask answered %q, %v", second, err)
	}
	if mints != 1 {
		t.Fatalf("a token with an hour left was minted %d times", mints)
	}

	// A cached token inside the last minute of its life is replaced rather
	// than handed out, and a cached expiry with no token behind it is never
	// trusted on the strength of its date alone.
	for _, cached := range []struct {
		name  string
		token string
		till  time.Duration
	}{
		{"a token inside its last minute", "nearly-spent", 30 * time.Second},
		{"a token that expired a second ago", "spent", -time.Second},
		{"an expiry with no token behind it", "", time.Hour},
	} {
		mints := 0
		installation := mintingInstallation(t, granting("fresh", time.Hour, &mints))
		installation.token, installation.tokenTill = cached.token, time.Now().Add(cached.till)
		token, err := installation.accessToken(context.Background())
		if err != nil || token != "fresh" {
			t.Errorf("%s answered %q, %v", cached.name, token, err)
		}
		if mints != 1 {
			t.Errorf("%s was minted %d times, want one replacement", cached.name, mints)
		}
	}
}

// A mint the endpoint answered badly is not a token: every shape is refused
// with nothing cached, so the next ask goes back to the endpoint rather than
// handing out a token that was never granted.
func TestAMintTheEndpointAnsweredBadlyIsNotAToken(t *testing.T) {
	for _, refusal := range []struct {
		name    string
		names   string
		handler http.HandlerFunc
	}{
		{"an ordinary 200 rather than a created 201", "returned HTTP 200", func(w http.ResponseWriter, _ *http.Request) {
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "t", ExpiresAt: time.Now().Add(time.Hour)})
		}},
		{"a revoked installation", "returned HTTP 401", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusUnauthorized)
		}},
		{"an endpoint that is down", "returned HTTP 503", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusServiceUnavailable)
		}},
		{"a body that is not a token document", "invalid installation token metadata", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_, _ = w.Write([]byte("<html>maintenance</html>"))
		}},
		{"a document with no token in it", "invalid installation token metadata", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{ExpiresAt: time.Now().Add(time.Hour)})
		}},
		{"a token that has already expired", "invalid installation token metadata", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "stale", ExpiresAt: time.Now().Add(-time.Second)})
		}},
		{"a token expiring inside the minute of margin", "invalid installation token metadata", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_ = json.NewEncoder(w).Encode(installationTokenResponse{Token: "fleeting", ExpiresAt: time.Now().Add(30 * time.Second)})
		}},
		{"a token document with no expiry at all", "invalid installation token metadata", func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusCreated)
			_, _ = w.Write([]byte(`{"token":"undated"}`))
		}},
		{"a connection that dropped mid-mint", "create GitHub installation token", func(w http.ResponseWriter, r *http.Request) {
			hangUp(w, r)
		}},
	} {
		asks := 0
		installation := mintingInstallation(t, func(w http.ResponseWriter, r *http.Request) {
			asks++
			refusal.handler(w, r)
		})
		token, err := installation.accessToken(context.Background())
		if err == nil || !strings.Contains(err.Error(), refusal.names) {
			t.Errorf("%s answered %v, want an error naming %q", refusal.name, err, refusal.names)
		}
		if token != "" {
			t.Errorf("%s answered the token %q", refusal.name, token)
		}
		if _, _ = installation.accessToken(context.Background()); asks != 2 {
			t.Errorf("%s was cached: the endpoint saw %d asks across two calls", refusal.name, asks)
		}
	}
}

// A key file that passes every check on its metadata and still cannot be
// opened is reported as the failed open it was, naming the file, rather than
// as a key that is not private or not PEM.
func TestAKeyFileThatCannotBeOpenedIsNamedAsAFailedOpen(t *testing.T) {
	sealed := soundKeyFile(t)
	if _, err := loadAppPrivateKey(sealed); err != nil {
		t.Fatalf("a sound key file was refused: %v", err)
	}
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })

	key, err := loadAppPrivateKey(sealed)
	if err == nil {
		t.Fatal("an unopenable key file loaded a key")
	}
	if !strings.Contains(err.Error(), "open GitHub App private key") {
		t.Fatalf("an unopenable key file answered %v", err)
	}
	if key != nil {
		t.Fatal("an unopenable key file answered a key as well as an error")
	}
}
