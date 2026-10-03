// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"crypto/tls"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// LoginAppRole is the only door through which a Terraform process gets a Vault
// token, and every branch below it is a refusal. The refusal an operator reads
// decides where they look: their own configuration, their own credential
// files, or the Vault deployment. These tests hold each refusal to its own
// cause, under a Vault endpoint the test controls end to end.

const appRoleGoodAnswer = `{"auth":{"client_token":"hvs.test-token-123456","lease_duration":300,` +
	`"renewable":true,"token_policies":["ra8ci"]}}`

// appRoleDoor starts a Vault stand-in under a certificate issued by the
// authority it returns, so the trust file is never the thing under test.
func appRoleDoor(t *testing.T, handler http.HandlerFunc) (*httptest.Server, []byte) {
	t.Helper()
	authority, leaf := vaultChain(t)
	server := httptest.NewUnstartedServer(handler)
	server.TLS = &tls.Config{Certificates: []tls.Certificate{leaf}, MinVersion: tls.VersionTLS12}
	server.StartTLS()
	return server, authority
}

// appRoleAnswering serves the login path with one status and one body.
func appRoleAnswering(t *testing.T, status int, body string) (*httptest.Server, []byte) {
	t.Helper()
	return appRoleDoor(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/bao/v1/auth/approle/login" {
			http.NotFound(w, r)
			return
		}
		if status != http.StatusOK {
			w.WriteHeader(status)
		}
		_, _ = w.Write([]byte(body))
	})
}

func appRoleRefused(t *testing.T, name string, config AppRoleConfig, want string) {
	t.Helper()
	token, err := LoginAppRole(context.Background(), config)
	if err == nil || token != nil {
		t.Fatalf("%s: expected a refusal, got token %v error %v", name, token, err)
	}
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("%s: expected a refusal naming %q, got %q", name, want, err.Error())
	}
}

func TestTheAppRoleConfigurationGuardRefusesWhatTheOperatorOwns(t *testing.T) {
	server, authority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
	defer server.Close()
	cases := []struct {
		name string
		edit func(*AppRoleConfig)
	}{
		{"padded address", func(c *AppRoleConfig) { c.Address = " " + c.Address }},
		{"trailing newline in the address", func(c *AppRoleConfig) { c.Address += "\n" }},
		{"no role ID file", func(c *AppRoleConfig) { c.RoleIDFile = "" }},
		{"no secret ID file", func(c *AppRoleConfig) { c.SecretIDFile = "" }},
		{"no CA file", func(c *AppRoleConfig) { c.CAFile = "" }},
		{"no auth mount", func(c *AppRoleConfig) { c.AuthMount = "" }},
		{"mount outside auth/", func(c *AppRoleConfig) { c.AuthMount = "approle" }},
		{"bare auth/", func(c *AppRoleConfig) { c.AuthMount = "auth/" }},
		{"empty mount segment", func(c *AppRoleConfig) { c.AuthMount = "auth//approle" }},
		{"trailing slash", func(c *AppRoleConfig) { c.AuthMount = "auth/approle/" }},
		{"space in the mount", func(c *AppRoleConfig) { c.AuthMount = "auth/app role" }},
		{"traversal in the mount", func(c *AppRoleConfig) { c.AuthMount = "auth/../sys" }},
		{"absolute mount", func(c *AppRoleConfig) { c.AuthMount = "/auth/approle" }},
	}
	for _, testCase := range cases {
		config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
		testCase.edit(&config)
		appRoleRefused(t, testCase.name, config, "invalid AppRole configuration")
	}
	// A nested mount path is still admitted: the pattern bounds the shape, not
	// the depth an operator may mount AppRole at.
	nested := appRoleConfigWithCA(t, server.URL+"/bao", authority)
	nested.AuthMount = "auth/ra8ci/approle"
	appRoleRefused(t, "nested mount reaches the endpoint", nested, "HTTP status 404")
}

func TestOnlyAnApprovedHTTPSOriginIsEverCalled(t *testing.T) {
	var called bool
	server, authority := appRoleDoor(t, func(w http.ResponseWriter, _ *http.Request) {
		called = true
		_, _ = w.Write([]byte(appRoleGoodAnswer))
	})
	defer server.Close()
	host := strings.TrimPrefix(server.URL, "https://")
	for name, address := range map[string]string{
		"plain HTTP":          "http://" + host + "/bao",
		"no scheme":           host + "/bao",
		"no port":             "https://vault.internal/bao",
		"credentials in it":   "https://user:pass@" + host + "/bao",
		"a query string":      "https://" + host + "/bao?ns=root",
		"a fragment":          "https://" + host + "/bao#token",
		"no host at all":      "https://",
		"a tailnet name":      "https://node.example.ts.net:8200/bao",
		"a tailnet address":   "https://100.64.10.5:8200/bao",
		"an unparseable host": "https://[::1/bao",
	} {
		config := appRoleConfigWithCA(t, address, authority)
		appRoleRefused(t, name, config, "approved HTTPS origin")
	}
	if called {
		t.Fatal("an unapproved origin still reached an endpoint")
	}
}

func TestAPersonalTailnetIsNeverAnApprovedVaultHost(t *testing.T) {
	// The control plane's Vault is operator-run infrastructure. A tailnet name
	// or a CGNAT address means somebody pointed it at their own machine.
	for _, personal := range []string{
		"node.ts.net", "NODE.TS.NET", "vault.tail1234.ts.net",
		"100.64.0.0", "100.100.100.100", "100.127.255.255", "::ffff:100.64.0.1",
	} {
		if !personalAppRoleHost(personal) {
			t.Fatalf("%q was not read as a personal host", personal)
		}
	}
	for _, shared := range []string{
		"vault.internal", "ts.net", "myts.net", "10.0.0.1", "192.168.1.10",
		"100.63.255.255", "100.128.0.0", "::1", "", "vault.ts.net.example.com",
	} {
		if personalAppRoleHost(shared) {
			t.Fatalf("%q was read as a personal host", shared)
		}
	}
}

func TestTheAppRoleTimeoutStaysInsideBoundedPolicy(t *testing.T) {
	server, authority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
	defer server.Close()
	for _, refused := range []time.Duration{
		-time.Second, time.Nanosecond, time.Microsecond,
		time.Millisecond - time.Nanosecond, 30*time.Second + time.Nanosecond, time.Minute,
	} {
		config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
		config.Timeout = refused
		appRoleRefused(t, refused.String(), config, "outside bounded policy")
	}
	// An unstated timeout is ten seconds, not "no deadline": the client the
	// token carries is the one every later revocation goes out on.
	config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
	config.Timeout = 0
	token, err := LoginAppRole(context.Background(), config)
	if err != nil {
		t.Fatalf("an unstated timeout was refused: %v", err)
	}
	defer token.Clear()
	if token.client.Timeout != 10*time.Second {
		t.Fatalf("expected the default ten-second timeout, got %v", token.client.Timeout)
	}
	if token.lease != 5*time.Minute {
		t.Fatalf("expected the answered five-minute lease, got %v", token.lease)
	}
}

func TestACredentialFileTheOperatorMisconfiguredNeverReachesTheEndpoint(t *testing.T) {
	var called bool
	server, authority := appRoleDoor(t, func(w http.ResponseWriter, _ *http.Request) {
		called = true
		_, _ = w.Write([]byte(appRoleGoodAnswer))
	})
	defer server.Close()
	cases := []struct {
		name  string
		want  string
		apply func(*testing.T, *AppRoleConfig)
	}{
		{"absent role ID file", "read AppRole role ID", func(t *testing.T, c *AppRoleConfig) {
			if err := os.Remove(c.RoleIDFile); err != nil {
				t.Fatal(err)
			}
		}},
		{"a directory where the role ID belongs", "read AppRole role ID", func(t *testing.T, c *AppRoleConfig) {
			if err := os.Remove(c.RoleIDFile); err != nil {
				t.Fatal(err)
			}
			if err := os.Mkdir(c.RoleIDFile, 0o700); err != nil {
				t.Fatal(err)
			}
		}},
		{"a role ID past the read bound", "read AppRole role ID", func(t *testing.T, c *AppRoleConfig) {
			if err := os.WriteFile(c.RoleIDFile, []byte(strings.Repeat("r", 257)), 0o600); err != nil {
				t.Fatal(err)
			}
		}},
		{"an empty role ID", "invalid contents", func(t *testing.T, c *AppRoleConfig) {
			if err := os.WriteFile(c.RoleIDFile, []byte("\n"), 0o600); err != nil {
				t.Fatal(err)
			}
		}},
		{"a role ID with a space in it", "invalid contents", func(t *testing.T, c *AppRoleConfig) {
			if err := os.WriteFile(c.RoleIDFile, []byte("role id 1234567890\n"), 0o600); err != nil {
				t.Fatal(err)
			}
		}},
		{"a secret ID one byte short", "invalid contents", func(t *testing.T, c *AppRoleConfig) {
			if err := os.WriteFile(c.SecretIDFile, []byte(strings.Repeat("s", 15)), 0o600); err != nil {
				t.Fatal(err)
			}
		}},
		{"a secret ID carrying a shell character", "invalid contents", func(t *testing.T, c *AppRoleConfig) {
			if err := os.WriteFile(c.SecretIDFile, []byte("secret-id-1234567890$(id)"), 0o600); err != nil {
				t.Fatal(err)
			}
		}},
	}
	for _, testCase := range cases {
		config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
		testCase.apply(t, &config)
		appRoleRefused(t, testCase.name, config, testCase.want)
	}
	if called {
		t.Fatal("a credential the guard refused was still sent to the endpoint")
	}

	// The surrounding whitespace a file written by an operator's editor carries
	// is trimmed rather than refused, and the sixteen bytes left after trimming
	// are the shortest secret accepted: fifteen is refused as invalid contents.
	accepted := appRoleConfigWithCA(t, server.URL+"/bao", authority)
	if err := os.WriteFile(accepted.RoleIDFile, []byte("  role-id-1234567890  \n\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(accepted.SecretIDFile, []byte("\tsecret-id-123456\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	token, err := LoginAppRole(context.Background(), accepted)
	if err != nil {
		t.Fatalf("a trimmed credential pair was refused: %v", err)
	}
	token.Clear()
}

func TestALoginTheEndpointRejectsNamesItsOwnStatus(t *testing.T) {
	for _, status := range []int{
		http.StatusBadRequest, http.StatusForbidden,
		http.StatusInternalServerError, http.StatusServiceUnavailable,
	} {
		server, authority := appRoleAnswering(t, status, `{"errors":["permission denied"]}`)
		config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
		appRoleRefused(t, http.StatusText(status), config, "AppRole login rejected with HTTP status")
		server.Close()
	}
}

func TestAnAnswerThatIsNotAUsableTokenIsRefused(t *testing.T) {
	cases := []struct {
		name string
		body string
		want string
	}{
		{"not JSON at all", "Vault sealed", "response is malformed"},
		{"an array", `[{"auth":{}}]`, "response is malformed"},
		{"a second document after it", appRoleGoodAnswer + `{"auth":{}}`, "trailing data"},
		{"no auth object", `{"warnings":[]}`, "invalid or overlong token"},
		{"a null auth object", `{"auth":null}`, "invalid or overlong token"},
		{"an empty token", `{"auth":{"client_token":"","lease_duration":300}}`, "invalid or overlong token"},
		{"a token below sixteen bytes", `{"auth":{"client_token":"hvs.short","lease_duration":300}}`, "invalid or overlong token"},
		{"a token with a space in it", `{"auth":{"client_token":"hvs.test token 123456","lease_duration":300}}`, "invalid or overlong token"},
		{"no lease at all", `{"auth":{"client_token":"hvs.test-token-123456"}}`, "invalid or overlong token"},
		{"a lease of zero", `{"auth":{"client_token":"hvs.test-token-123456","lease_duration":0}}`, "invalid or overlong token"},
		{"a negative lease", `{"auth":{"client_token":"hvs.test-token-123456","lease_duration":-1}}`, "invalid or overlong token"},
		{"a lease past one hour", `{"auth":{"client_token":"hvs.test-token-123456","lease_duration":3601}}`, "invalid or overlong token"},
		{"an answer past the read bound", `{"auth":{"client_token":"hvs.` + strings.Repeat("a", 70000) + `"}}`, "unreadable or oversized"},
	}
	for _, testCase := range cases {
		server, authority := appRoleAnswering(t, http.StatusOK, testCase.body)
		config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
		appRoleRefused(t, testCase.name, config, testCase.want)
		server.Close()
	}

	// The exact bounds of the lease policy, both admitted.
	for _, lease := range []string{"1", "3600"} {
		server, authority := appRoleAnswering(t, http.StatusOK,
			`{"auth":{"client_token":"hvs.test-token-123456","lease_duration":`+lease+`}}`)
		token, err := LoginAppRole(context.Background(),
			appRoleConfigWithCA(t, server.URL+"/bao", authority))
		if err != nil {
			t.Fatalf("a lease of %s seconds was refused: %v", lease, err)
		}
		token.Clear()
		server.Close()
	}
}

func TestTheRevocationDoorRefusesATokenItCannotUse(t *testing.T) {
	var nilToken *AppRoleToken
	if err := nilToken.Revoke(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "token is unavailable") {
		t.Fatalf("expected a nil token to be unavailable, got %v", err)
	}
	if got := nilToken.String(); got != "AppRoleToken{nil}" {
		t.Fatalf("nil token String() = %q", got)
	}
	if _, err := nilToken.TerraformEnvironment(); err == nil {
		t.Fatal("a nil token produced a Terraform environment")
	}
	nilToken.Clear() // must not panic

	server, authority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
	defer server.Close()
	token, err := LoginAppRole(context.Background(),
		appRoleConfigWithCA(t, server.URL+"/bao", authority))
	if err != nil {
		t.Fatalf("the reviewed login was refused: %v", err)
	}
	if err := token.Revoke(nil); err == nil || //nolint:staticcheck // the nil context is the case under test
		!strings.Contains(err.Error(), "token is unavailable") {
		t.Fatalf("expected a nil context to be refused, got %v", err)
	}
	if strings.Contains(token.String(), "hvs.test-token-123456") {
		t.Fatalf("the token value reached its own String(): %q", token.String())
	}
	token.Clear()
	if err := token.Revoke(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "token is unavailable") {
		t.Fatalf("expected a cleared token to be unavailable, got %v", err)
	}
}

func TestARevocationTheEndpointRefusesOrNeverAnswersIsReported(t *testing.T) {
	refusing, authority := appRoleDoor(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/bao/v1/auth/approle/login" {
			_, _ = w.Write([]byte(appRoleGoodAnswer))
			return
		}
		w.WriteHeader(http.StatusInternalServerError)
	})
	defer refusing.Close()
	token, err := LoginAppRole(context.Background(),
		appRoleConfigWithCA(t, refusing.URL+"/bao", authority))
	if err != nil {
		t.Fatalf("the reviewed login was refused: %v", err)
	}
	if err := token.Revoke(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "revocation returned HTTP status 500") {
		t.Fatalf("expected the revocation status to be named, got %v", err)
	}
	token.Clear()

	unreachable, unreachableAuthority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
	second, err := LoginAppRole(context.Background(),
		appRoleConfigWithCA(t, unreachable.URL+"/bao", unreachableAuthority))
	if err != nil {
		t.Fatalf("the reviewed login was refused: %v", err)
	}
	unreachable.Close()
	if err := second.Revoke(context.Background()); err == nil ||
		!strings.Contains(err.Error(), "token revocation failed") {
		t.Fatalf("expected an unanswered revocation to be reported, got %v", err)
	}
	// The token value is still the caller's to clear: a failed revocation
	// leaves the TTL as the only fence, and Clear is what removes the copy.
	second.Clear()
	if _, err := second.TerraformEnvironment(); err == nil {
		t.Fatal("a cleared token still produced a Terraform environment")
	}
}

func TestTheLoginPathIsBuiltFromTheMountTheOperatorNamed(t *testing.T) {
	var paths []string
	server, authority := appRoleDoor(t, func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.URL.Path)
		_, _ = w.Write([]byte(appRoleGoodAnswer))
	})
	defer server.Close()
	config := appRoleConfigWithCA(t, server.URL+"/bao/", authority)
	config.AuthMount = "auth/ra8ci-approle"
	token, err := LoginAppRole(context.Background(), config)
	if err != nil {
		t.Fatalf("the reviewed login was refused: %v", err)
	}
	if err := token.Revoke(context.Background()); err != nil {
		t.Fatalf("revocation was refused: %v", err)
	}
	token.Clear()
	want := []string{"/bao/v1/auth/ra8ci-approle/login", "/bao/v1/auth/token/revoke-self"}
	if len(paths) != len(want) || paths[0] != want[0] || paths[1] != want[1] {
		t.Fatalf("expected %v, the endpoint saw %v", want, paths)
	}
	if directory := filepath.Dir(config.CAFile); directory == "" {
		t.Fatal("the trust file has no directory")
	}
}
