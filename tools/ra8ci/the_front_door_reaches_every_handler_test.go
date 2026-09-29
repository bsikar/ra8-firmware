// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"io"
	"os"
	"strings"
	"testing"
)

// The dispatch table is the only thing standing between a word an operator
// types and the catalog: run() looks the word up, and anything it does not
// find is handed to runLocalTask as a task name. So a command dropped from the
// table does not fail loudly, it quietly becomes "unknown task", which reads
// like the operator's mistake rather than a missing command. Every word is
// driven through the real front door here, and none of them may answer that
// way.

// throughTheFrontDoor runs one invocation with both streams captured, so a
// command that writes a catalog listing or a refusal does not land in the test
// output, and both halves can be read back.
func throughTheFrontDoor(t *testing.T, args []string) (string, string, int) {
	t.Helper()
	outReader, outWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	errReader, errWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	savedOut, savedErr := os.Stdout, os.Stderr
	os.Stdout, os.Stderr = outWriter, errWriter

	// Read both pipes while the command runs: a command that writes more than
	// a pipe buffer holds would otherwise block forever on the write.
	spoke := make(chan string, 1)
	complained := make(chan string, 1)
	go func() { body, _ := io.ReadAll(outReader); spoke <- string(body) }()
	go func() { body, _ := io.ReadAll(errReader); complained <- string(body) }()

	status := run(context.Background(), args)

	os.Stdout, os.Stderr = savedOut, savedErr
	if err := outWriter.Close(); err != nil {
		t.Fatal(err)
	}
	if err := errWriter.Close(); err != nil {
		t.Fatal(err)
	}
	return <-spoke, <-complained, status
}

// offline empties every credential the plane-facing commands read, so nothing
// driven here can reach a network, and points local state somewhere private.
func offline(t *testing.T) {
	t.Helper()
	for _, name := range []string{
		envServerURL, envServerCA, envClientCert, envClientKey,
		"RA8CI_DATABASE_URL", "RA8CI_MIGRATION_DATABASE_URL", "RA8CI_TLS_CERT",
	} {
		t.Setenv(name, "")
	}
	privateOutbox(t)
}

func TestEveryWordTheFrontDoorNamesReachesItsOwnHandler(t *testing.T) {
	// One safe invocation per command: enough to reach the handler, chosen so
	// each refuses on its own terms rather than doing any real work. An
	// unknown flag is refused by a gate before it reads anything, and the
	// plane-facing commands are refused by the missing credentials above.
	invocations := map[string][]string{
		"tasks":                         nil,
		"server":                        nil,
		"agent":                         nil,
		"board-agent":                   nil,
		"sync":                          nil,
		"github":                        nil,
		"backup":                        nil,
		"board":                         nil,
		"hil":                           nil,
		"report":                        nil,
		"run":                           nil,
		"db":                            {"migrate"},
		"ascii":                         {"--nope"},
		"since":                         {"--nope"},
		"final-newline":                 {"--nope"},
		"runner-clock":                  {"--nope"},
		"tests-readme":                  {"--nope"},
		"inclusive-terminology-commits": {"--nope"},
	}
	// The table is the source of truth, so every command in it has to be
	// covered here. A command added without an invocation fails this.
	for _, command := range topLevelCommands() {
		if _, named := invocations[command.Name]; !named {
			t.Fatalf("%s is dispatched but is not driven here", command.Name)
		}
	}

	for name, arguments := range invocations {
		t.Run(name, func(t *testing.T) {
			offline(t)
			_, complained, _ := throughTheFrontDoor(t, append([]string{name}, arguments...))

			// The tell that a word fell through the table to the catalog.
			if strings.Contains(complained, "unknown task "+name) {
				t.Fatalf("%s was handed to the catalog: %q", name, complained)
			}
		})
	}
}

func TestAWordTheTableDoesNotHoldIsTriedAsATask(t *testing.T) {
	offline(t)
	// The other half of the same seam: a word that is genuinely not a command
	// has to reach the catalog and be refused there by name, not silently.
	_, complained, status := throughTheFrontDoor(t, []string{"not-a-command"})
	if status != 2 {
		t.Fatalf("exit=%d; want the catalog's refusal", status)
	}
	if !strings.Contains(complained, "unknown task not-a-command") {
		t.Fatalf("stderr=%q; want the catalog to name what it could not find", complained)
	}
}

func TestTheFrontDoorWithNoWordAtAllStatesItsUsage(t *testing.T) {
	offline(t)
	spoke, complained, status := throughTheFrontDoor(t, nil)
	if status != 2 {
		t.Fatalf("exit=%d; want the usage refusal", status)
	}
	if !strings.Contains(complained, usageLine()) {
		t.Fatalf("stderr=%q; want the usage line", complained)
	}
	// Usage is a refusal, and a refusal belongs on stderr so a caller piping
	// the command's answer somewhere does not receive it as one.
	if strings.TrimSpace(spoke) != "" {
		t.Fatalf("stdout=%q; want usage kept off the answer stream", spoke)
	}
}

func TestGithubNamesEverySubcommandWhenItCannotDispatch(t *testing.T) {
	offline(t)
	// A wrong arity and an unknown name are answered the same way, and both
	// have to state the whole table: the usage is how an operator who typed
	// the wrong one finds the right one.
	for _, args := range [][]string{nil, {"shadow", "extra"}, {"not-a-subcommand"}} {
		err := githubCommand(context.Background(), args)
		if err == nil {
			t.Fatalf("github %v was dispatched", args)
		}
		for _, subcommand := range githubSubcommands() {
			if !strings.Contains(err.Error(), subcommand.Name) {
				t.Fatalf("github %v: usage omits %s: %v", args, subcommand.Name, err)
			}
		}
	}
}

func TestEveryGithubSubcommandIsReachableByTheNameItIsStatedUnder(t *testing.T) {
	// Dispatch is by exact name against the same table the usage is built
	// from, so the two can never disagree. Checking the lookup rather than
	// running each subcommand keeps this offline: several of them open a
	// GitHub session, which is not this test's business.
	for _, subcommand := range githubSubcommands() {
		if subcommand.Name == "" {
			t.Fatal("a GitHub subcommand has no name")
		}
		if subcommand.Run == nil {
			t.Fatalf("github %s runs nothing", subcommand.Name)
		}
		if strings.ToLower(subcommand.Name) != subcommand.Name {
			t.Fatalf("github %s cannot be typed as stated", subcommand.Name)
		}
	}
}
