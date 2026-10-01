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
	"strings"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v4"
)

// A GitHub App installation token is minted the same way wherever this plane
// needs one: an RS256 JWT signed with the App key, posted to
// /app/installations/<id>/access_tokens, scoped to one repository, and cached
// until shortly before it expires. What differs between callers is the
// PERMISSION SET, and that difference is the whole point of minting separately:
// the metadata resolver reads the Actions API and must never hold a token that
// could write a check run, and the check-run publisher writes checks and must
// never hold one that could read another repository's Actions history.
//
// This file states the minting once, parameterised by permissions, so the two
// callers differ in exactly the field that should differ and in nothing else.
// It replaces the copy that lived in metadata.go; MetadataResolver delegates
// and its behaviour is unchanged.

// appInstallation mints and caches one installation token for one repository
// under one fixed permission set.
type appInstallation struct {
	apiURL         *url.URL
	key            *rsa.PrivateKey
	client         *http.Client
	clientID       string
	installationID int64
	repository     string
	permissions    map[string]string

	mu        sync.Mutex
	token     string
	tokenTill time.Time
}

// accessToken returns a cached installation token, minting a new one when the cached
// one is within a minute of expiry.
func (a *appInstallation) accessToken(ctx context.Context) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.token != "" && time.Until(a.tokenTill) > time.Minute {
		return a.token, nil
	}
	issued := time.Now().Add(-time.Minute)
	claims := jwt.RegisteredClaims{Issuer: a.clientID,
		IssuedAt: jwt.NewNumericDate(issued), ExpiresAt: jwt.NewNumericDate(issued.Add(8 * time.Minute))}
	appJWT, err := jwt.NewWithClaims(jwt.SigningMethodRS256, claims).SignedString(a.key)
	if err != nil {
		return "", errors.New("sign GitHub App authentication token")
	}
	endpoint := *a.apiURL
	endpoint.Path = path.Join(endpoint.Path, "app", "installations", fmt.Sprint(a.installationID), "access_tokens")
	payload, _ := json.Marshal(installationTokenRequest{Repositories: []string{a.repository}, Permissions: a.permissions})
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint.String(), strings.NewReader(string(payload)))
	if err != nil {
		return "", errors.New("build GitHub App installation token request")
	}
	request.Header.Set("Accept", "application/vnd.github+json")
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+appJWT)
	request.Header.Set("X-GitHub-Api-Version", githubAPIVersion)
	response, err := a.client.Do(request)
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
	a.token, a.tokenTill = token.Token, token.ExpiresAt
	return a.token, nil
}

// validAppAPIOrigin restricts an App API base URL to exactly the public
// api.github.com HTTPS origin, with no path, query, fragment or userinfo. It is
// the rule NewMetadataResolver already applied, stated once so a second caller
// cannot admit an origin the first refuses.
func validAppAPIOrigin(base string) (*url.URL, error) {
	if base == "" {
		base = "https://api.github.com"
	}
	apiURL, err := url.Parse(base)
	if err != nil || apiURL == nil || !strings.EqualFold(apiURL.Scheme, "https") ||
		!strings.EqualFold(apiURL.Hostname(), "api.github.com") ||
		(apiURL.Port() != "" && apiURL.Port() != "443") || apiURL.User != nil ||
		apiURL.Path != "" || apiURL.RawPath != "" || apiURL.RawQuery != "" || apiURL.ForceQuery ||
		apiURL.Fragment != "" || strings.TrimSpace(base) != base {
		return nil, errors.New("GitHub App API base URL must be exactly the public api.github.com HTTPS origin")
	}
	return apiURL, nil
}

// loadAppPrivateKey reads an RSA App key from a private, bounded, regular file.
// The file is stat'd, opened, and stat'd again through the open handle, and the
// two are required to be the same file with the same private mode, so a key
// swapped between the check and the read is refused rather than loaded.
func loadAppPrivateKey(file string) (*rsa.PrivateKey, error) {
	keyInfo, err := os.Lstat(file)
	if err != nil {
		return nil, fmt.Errorf("stat GitHub App private key: %w", err)
	}
	if !keyInfo.Mode().IsRegular() || keyInfo.Size() < 1 || keyInfo.Size() > maxGitHubPrivateKeyBytes || keyInfo.Mode().Perm()&0077 != 0 {
		return nil, errors.New("GitHub App private key must be a private bounded regular file")
	}
	keyFile, err := os.Open(file)
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
	return privateKey, nil
}

// appHTTPClient returns the client this plane talks to GitHub with: no proxy,
// a bounded timeout, and redirects returned rather than followed, so a
// redirected request never carries an installation token to another origin. A
// non-nil seam is a deterministic-test client and is copied rather than
// mutated.
func appHTTPClient(seam *http.Client) *http.Client {
	client := seam
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
	return client
}
