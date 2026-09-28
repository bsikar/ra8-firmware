// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"net/http"
	"os"
	"strings"
	"testing"
)

// A Vault login fails in three different places: the endpoint may never
// answer, it may answer with more than the plane will read, or a credential
// file may satisfy the file policy and still not open. Each of those sends an
// operator somewhere different, so each has to keep its own refusal.

// TestAVaultEndpointThatNeverAnswersIsNotACredentialProblem pins that a dead
// endpoint reads as a transport failure. A caller told the credential was bad
// rotates a role ID that was never the problem.
func TestAVaultEndpointThatNeverAnswersIsNotACredentialProblem(t *testing.T) {
	server, authority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
	config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
	server.Close()

	appRoleRefused(t, "endpoint closed", config, "AppRole login request failed")
}

// TestAnAnswerLargerThanThePlaneWillReadIsRefusedWhole pins the read bound on
// the login answer. The body is read before the status is judged, so an
// endpoint that floods the reader is refused as an unusable answer rather than
// being allowed to spend memory on the way to a status check.
func TestAnAnswerLargerThanThePlaneWillReadIsRefusedWhole(t *testing.T) {
	flood := strings.Repeat("a", maxAppRoleResponseBytes+1)
	server, authority := appRoleAnswering(t, http.StatusOK,
		`{"auth":{"client_token":"hvs.`+flood+`"}}`)
	defer server.Close()

	appRoleRefused(t, "oversized answer", appRoleConfigWithCA(t, server.URL+"/bao", authority),
		"AppRole login response is unreadable or oversized")

	// The bound is on the answer, not on the token: an ordinary answer of
	// the same shape is still accepted, so this refusal cannot be read as
	// the plane refusing long bodies in general.
	ok, okAuthority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
	defer ok.Close()
	token, err := LoginAppRole(context.Background(), appRoleConfigWithCA(t, ok.URL+"/bao", okAuthority))
	if err != nil {
		t.Fatalf("an ordinary answer was refused: %v", err)
	}
	token.Clear()
}

// TestACredentialFileThePolicyAdmitsButCannotBeOpened pins the gap between the
// file policy and the read. A mode 0o000 file is a regular file, inside the
// size bound, with no group or other bits, so it passes every policy check and
// still cannot be opened. Each of the three credentials keeps its own wrapper,
// which is what tells an operator WHICH file to look at.
func TestACredentialFileThePolicyAdmitsButCannotBeOpened(t *testing.T) {
	for name, pick := range map[string]func(AppRoleConfig) (string, string){
		"role ID":   func(c AppRoleConfig) (string, string) { return c.RoleIDFile, "read AppRole role ID" },
		"secret ID": func(c AppRoleConfig) (string, string) { return c.SecretIDFile, "read AppRole secret ID" },
		"CA bundle": func(c AppRoleConfig) (string, string) { return c.CAFile, "read AppRole CA bundle" },
	} {
		t.Run(name, func(t *testing.T) {
			server, authority := appRoleAnswering(t, http.StatusOK, appRoleGoodAnswer)
			defer server.Close()
			config := appRoleConfigWithCA(t, server.URL+"/bao", authority)
			file, wrapper := pick(config)
			if err := os.Chmod(file, 0o000); err != nil {
				t.Fatalf("seal credential file: %v", err)
			}
			appRoleRefused(t, name, config, wrapper)
			appRoleRefused(t, name, config, "credential file is unreadable")
		})
	}
}
