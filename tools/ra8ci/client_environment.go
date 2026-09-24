// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"fmt"
	"strings"
)

// The CLI has no flags for its mutual-TLS surface: every command reads the
// same environment, and until now each one read it for itself. Six call sites
// spelled the names out six times, two of them omitted the check entirely and
// let an opaque client-construction error stand in for it, and each refusal
// listed every variable the command wanted whether or not it was the one the
// operator had missed.
//
// The names are stated here once, grouped by the identity a command presents.
// That grouping is the point: #1476 splits the client certificate from the
// agent certificate at the server, and a command that reaches for the wrong
// pair should be reading the wrong constant here rather than the right-looking
// string somewhere else.
const (
	envServerURL      = "RA8CI_SERVER_URL"
	envServerCA       = "RA8CI_SERVER_CA"
	envClientCert     = "RA8CI_CLIENT_CERT"
	envClientKey      = "RA8CI_CLIENT_KEY"
	envAgentCert      = "RA8CI_AGENT_CERT"
	envAgentKey       = "RA8CI_AGENT_KEY"
	envAgentRoot      = "RA8CI_AGENT_ROOT"
	envBoardAgentCert = "RA8CI_BOARD_AGENT_CERT"
	envBoardAgentKey  = "RA8CI_BOARD_AGENT_KEY"
	envBoardID        = "RA8CI_BOARD_ID"
	envBoardStateFile = "RA8CI_BOARD_STATE_FILE"
)

// clientRole is the identity a command presents on the wire. The server
// decides what a certificate may do; this decides only which key pair a
// command is entitled to read from the environment.
type clientRole struct {
	name    string
	certEnv string
	keyEnv  string
}

var (
	// roleOperator is the human or CI caller of the client API: run
	// submission and reads, board commands, offline sync, reports.
	roleOperator = clientRole{name: "client", certEnv: envClientCert, keyEnv: envClientKey}
	// roleRunnerAgent is the runner VM claiming and executing work.
	roleRunnerAgent = clientRole{name: "agent", certEnv: envAgentCert, keyEnv: envAgentKey}
	// roleBoardAgent is the board-attached agent, which holds a lease and
	// never executes a task.
	roleBoardAgent = clientRole{name: "board-agent", certEnv: envBoardAgentCert, keyEnv: envBoardAgentKey}
)

// clientEndpoint is a resolved mutual-TLS surface: where to connect, whom to
// trust, and which identity to present.
type clientEndpoint struct {
	ServerURL string
	CAFile    string
	CertFile  string
	KeyFile   string
}

// environmentValue treats a blank value as absent. An empty string already
// read as unset; a value that is only whitespace is a mistake that would
// otherwise reach os.ReadFile as a path and fail as a missing file.
func environmentValue(lookup func(string) string, name string) string {
	if lookup == nil {
		return ""
	}
	value := lookup(name)
	if strings.TrimSpace(value) == "" {
		return ""
	}
	return value
}

// missingEnvironment reports the names with no value, in the order given.
func missingEnvironment(lookup func(string) string, names ...string) []string {
	var missing []string
	for _, name := range names {
		if environmentValue(lookup, name) == "" {
			missing = append(missing, name)
		}
	}
	return missing
}

// environmentError names exactly what the operator has to set, and nothing
// they have already set. A refusal that lists the whole surface every time
// makes the operator re-check variables that were never the problem.
func environmentError(command string, missing []string) error {
	switch len(missing) {
	case 0:
		return nil
	case 1:
		return fmt.Errorf("%s: set %s", command, missing[0])
	default:
		return fmt.Errorf("%s: set %s and %s", command,
			strings.Join(missing[:len(missing)-1], ", "), missing[len(missing)-1])
	}
}

// resolveClientEndpoint reads the surface for one role. Additional names a
// command needs are checked in the same pass, so an operator setting a command
// up for the first time is told everything that is missing at once rather than
// one variable per attempt.
func resolveClientEndpoint(command string, role clientRole, lookup func(string) string, also ...string) (clientEndpoint, error) {
	names := append([]string{envServerURL, envServerCA, role.certEnv, role.keyEnv}, also...)
	if missing := missingEnvironment(lookup, names...); len(missing) > 0 {
		return clientEndpoint{}, environmentError(command, missing)
	}
	return clientEndpoint{
		ServerURL: environmentValue(lookup, envServerURL),
		CAFile:    environmentValue(lookup, envServerCA),
		CertFile:  environmentValue(lookup, role.certEnv),
		KeyFile:   environmentValue(lookup, role.keyEnv),
	}, nil
}
