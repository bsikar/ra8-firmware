// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

func lookupFrom(values map[string]string) func(string) string {
	return func(name string) string { return values[name] }
}

func completeOperatorEnvironment() map[string]string {
	return map[string]string{
		envServerURL:  "https://ci.example",
		envServerCA:   "/etc/ra8ci/server-ca.pem",
		envClientCert: "/etc/ra8ci/client.pem",
		envClientKey:  "/etc/ra8ci/client.key",
	}
}

func TestResolveClientEndpointReadsTheRolesOwnKeyPair(t *testing.T) {
	values := completeOperatorEnvironment()
	values[envAgentCert] = "/etc/ra8ci/agent.pem"
	values[envAgentKey] = "/etc/ra8ci/agent.key"
	values[envAgentRoot] = "/srv/ra8ci"

	operator, err := resolveClientEndpoint("ra8ci run", roleOperator, lookupFrom(values))
	if err != nil {
		t.Fatalf("operator endpoint: %v", err)
	}
	if operator.CertFile != values[envClientCert] || operator.KeyFile != values[envClientKey] {
		t.Fatalf("operator endpoint took the wrong key pair: %+v", operator)
	}
	agent, err := resolveClientEndpoint("ra8ci agent", roleRunnerAgent, lookupFrom(values), envAgentRoot)
	if err != nil {
		t.Fatalf("agent endpoint: %v", err)
	}
	if agent.CertFile != values[envAgentCert] || agent.KeyFile != values[envAgentKey] {
		t.Fatalf("agent endpoint took the wrong key pair: %+v", agent)
	}
	// The server URL and trust root are shared; the identity is not. A role
	// that silently fell back to the client pair would still connect, and
	// would spend a grant the server split deliberately in #1476.
	if agent.ServerURL != operator.ServerURL || agent.CAFile != operator.CAFile {
		t.Fatalf("roles disagree about the server: %+v vs %+v", agent, operator)
	}
	if agent.CertFile == operator.CertFile || agent.KeyFile == operator.KeyFile {
		t.Fatalf("agent role reused the client identity: %+v", agent)
	}
}

func TestResolveClientEndpointRefusesTheAgentRoleWithOnlyAClientPair(t *testing.T) {
	_, err := resolveClientEndpoint("ra8ci agent", roleRunnerAgent, lookupFrom(completeOperatorEnvironment()), envAgentRoot)
	if err == nil {
		t.Fatal("agent role accepted an environment holding only a client key pair")
	}
	for _, name := range []string{envAgentCert, envAgentKey, envAgentRoot} {
		if !strings.Contains(err.Error(), name) {
			t.Fatalf("refusal does not name %s: %v", name, err)
		}
	}
	for _, name := range []string{envClientCert, envClientKey, envServerURL, envServerCA} {
		if strings.Contains(err.Error(), name) {
			t.Fatalf("refusal names %s, which is set: %v", name, err)
		}
	}
}

func TestResolveClientEndpointNamesOnlyWhatIsMissing(t *testing.T) {
	values := completeOperatorEnvironment()
	delete(values, envClientKey)
	_, err := resolveClientEndpoint("ra8ci sync", roleOperator, lookupFrom(values))
	if err == nil {
		t.Fatal("accepted an environment with no client key")
	}
	if err.Error() != "ra8ci sync: set "+envClientKey {
		t.Fatalf("unexpected refusal: %v", err)
	}
}

func TestResolveClientEndpointRefusesABlankValue(t *testing.T) {
	values := completeOperatorEnvironment()
	values[envServerCA] = "   "
	_, err := resolveClientEndpoint("ra8ci run", roleOperator, lookupFrom(values))
	if err == nil {
		t.Fatal("accepted a whitespace-only trust root path")
	}
	if !strings.Contains(err.Error(), envServerCA) {
		t.Fatalf("refusal does not name the blank variable: %v", err)
	}
}

func TestResolveClientEndpointKeepsAValueExactly(t *testing.T) {
	values := completeOperatorEnvironment()
	values[envClientCert] = "/etc/ra8ci/a path with spaces.pem"
	endpoint, err := resolveClientEndpoint("ra8ci run", roleOperator, lookupFrom(values))
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	if endpoint.CertFile != values[envClientCert] {
		t.Fatalf("value was altered: %q", endpoint.CertFile)
	}
}

func TestEnvironmentErrorListsEveryMissingName(t *testing.T) {
	values := map[string]string{envServerURL: "https://ci.example"}
	_, err := resolveClientEndpoint("ra8ci board-agent", roleBoardAgent, lookupFrom(values), envBoardID, envBoardStateFile)
	if err == nil {
		t.Fatal("accepted an almost empty environment")
	}
	want := "ra8ci board-agent: set " + strings.Join([]string{
		envServerCA, envBoardAgentCert, envBoardAgentKey, envBoardID,
	}, ", ") + " and " + envBoardStateFile
	if err.Error() != want {
		t.Fatalf("refusal reads %q, want %q", err.Error(), want)
	}
}

func TestClientRolesDoNotShareAnEnvironmentName(t *testing.T) {
	seen := map[string]string{}
	for _, role := range []clientRole{roleOperator, roleRunnerAgent, roleBoardAgent} {
		for _, name := range []string{role.certEnv, role.keyEnv} {
			if other, ok := seen[name]; ok {
				t.Fatalf("%s is read by both the %s and %s roles", name, other, role.name)
			}
			seen[name] = role.name
		}
	}
}
