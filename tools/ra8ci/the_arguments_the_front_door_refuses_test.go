// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// front_door_test.go states what the dispatch table IS. This states what it
// DOES with arguments it will not accept. The distinction matters because the
// commands below all start something expensive on the far side of their
// argument check: a listener, an agent's poll loop, a backup refresh. A
// refusal that arrives after the work has started is not a refusal.

// dispatch types one command line at the front door, without going through
// run(), so an unknown name cannot fall through to the local task runner and
// read the catalog.
func dispatch(t *testing.T, name string, arguments ...string) int {
	t.Helper()
	command, ok := topLevelCommandNamed(name)
	if !ok {
		t.Fatalf("%s is not dispatched", name)
	}
	return command.Run(context.Background(), arguments)
}

// Every long-running command takes no arguments, and each one is asked with a
// stray argument rather than with none: a command given none would start the
// thing under test.
func TestTheLongRunningCommandsRefuseAnArgumentBeforeTheyStart(t *testing.T) {
	for _, name := range []string{"server", "agent", "board-agent", "sync"} {
		t.Run(name, func(t *testing.T) {
			for _, arguments := range [][]string{
				{"--help"},
				{""},
				{"--once"},
				{"a", "b"},
			} {
				if status := dispatch(t, name, arguments...); status != 2 {
					t.Errorf("%s %q answered %d; want the usage refusal", name, arguments, status)
				}
			}
		})
	}
}

// db is the one top-level command that takes a single fixed word, and the
// word is checked before a database is opened.
func TestTheDatabaseCommandTakesMigrateAndNothingElse(t *testing.T) {
	for _, arguments := range [][]string{
		{},
		{"migrate", "now"},
		{"Migrate"},
		{"migrate "},
		{"up"},
		{""},
	} {
		if status := dispatch(t, "db", arguments...); status != 2 {
			t.Errorf("db %q answered %d; want the usage refusal", arguments, status)
		}
	}
}

// github dispatches exactly one subcommand per invocation, and says what it
// takes when it cannot.
func TestTheGitHubCommandNamesItsSubcommandsWhenItCannotDispatch(t *testing.T) {
	for name, arguments := range map[string][]string{
		"nothing to dispatch":  {},
		"two subcommands":      {"check", "shadow"},
		"an unknown one":       {"check-session"},
		"an empty one":         {""},
		"the wrong case":       {"Check"},
		"a trailing space":     {"check "},
		"the usage word alone": {"usage"},
	} {
		t.Run(name, func(t *testing.T) {
			err := githubCommand(context.Background(), arguments)
			if err == nil {
				t.Fatalf("github %q was dispatched", arguments)
			}
			if !strings.HasPrefix(err.Error(), "usage: ra8ci github ") {
				t.Fatalf("err=%q; want it to state the github usage", err)
			}
			// The refusal is built from the table, so a subcommand added
			// later is named by it without anybody editing a second list.
			for _, subcommand := range githubSubcommands() {
				if !strings.Contains(err.Error(), subcommand.Name) {
					t.Errorf("the refusal does not name %s: %s", subcommand.Name, err)
				}
			}
		})
	}
}

func TestTheBackupCommandTakesRefreshOrKeygenAndNothingElse(t *testing.T) {
	for _, arguments := range [][]string{
		{},
		{"refresh", "now"},
		{"Refresh"},
		{"KEYGEN"},
		{"restore"},
		{""},
	} {
		err := backupCommand(context.Background(), arguments)
		if err == nil || err.Error() != "usage: ra8ci backup refresh|keygen" {
			t.Errorf("backup %q answered %v; want the usage refusal", arguments, err)
		}
	}
}

// unsetSessionEnvironment clears every scale-set variable for the test and
// restores what was there afterwards. t.Setenv registers the restore; the
// unset immediately after it is what the check actually sees.
func unsetSessionEnvironment(t *testing.T) {
	t.Helper()
	for _, name := range []string{
		github.EnvConfigURL, github.EnvAppClientID, github.EnvInstallationID,
		github.EnvPrivateKeyFile, github.EnvOwner, github.EnvScaleSetID,
		github.EnvMaxRunners,
	} {
		t.Setenv(name, "placeholder")
		if err := os.Unsetenv(name); err != nil {
			t.Fatalf("clear %s: %v", name, err)
		}
	}
}

// A plane with no scale-set configuration at all says so, rather than trying
// to open a session against an empty address and reporting a network failure.
func TestTheSessionCheckSaysWhenScaleSetsAreNotConfigured(t *testing.T) {
	unsetSessionEnvironment(t)
	err := githubSessionCheck(context.Background())
	if err == nil || !strings.Contains(err.Error(), "not configured") {
		t.Fatalf("err=%v; want the unconfigured answer", err)
	}
}

// Half a configuration is the dangerous shape: it is enabled, so it must name
// the piece that is missing rather than fall back to not configured.
func TestTheSessionCheckNamesTheVariableAHalfConfigurationIsMissing(t *testing.T) {
	for _, present := range []string{
		github.EnvConfigURL, github.EnvOwner, github.EnvMaxRunners,
	} {
		t.Run(present, func(t *testing.T) {
			unsetSessionEnvironment(t)
			t.Setenv(present, "1")
			err := githubSessionCheck(context.Background())
			if err == nil {
				t.Fatal("a half configuration opened a session")
			}
			if strings.Contains(err.Error(), "not configured") {
				t.Fatalf("err=%q; a half configuration must not read as no configuration", err)
			}
		})
	}

	// Every variable present and one of them empty is enabled too, and the
	// empty one is named rather than carried into a session.
	t.Run("one of them empty", func(t *testing.T) {
		unsetSessionEnvironment(t)
		for _, name := range []string{
			github.EnvConfigURL, github.EnvAppClientID, github.EnvInstallationID,
			github.EnvPrivateKeyFile, github.EnvOwner, github.EnvScaleSetID,
			github.EnvMaxRunners,
		} {
			t.Setenv(name, "1")
		}
		t.Setenv(github.EnvOwner, "")
		err := githubSessionCheck(context.Background())
		if err == nil || !strings.Contains(err.Error(), github.EnvOwner) {
			t.Fatalf("err=%v; want %s named", err, github.EnvOwner)
		}
	})
}

// The two exit statuses the whole table funnels through. A command that
// reports nothing wrong is a zero, and the two failure kinds stay distinct:
// 2 is "you typed it wrong", 1 is "it went wrong".
func TestTheFrontDoorKeepsATypoAndAFailureApart(t *testing.T) {
	if status := reportError(nil); status != 0 {
		t.Errorf("nothing wrong answered %d", status)
	}
	if status := reportError(context.Canceled); status != 1 {
		t.Errorf("a failure answered %d", status)
	}
	if status := usageError("server takes no arguments"); status != 2 {
		t.Errorf("a typo answered %d", status)
	}
}
