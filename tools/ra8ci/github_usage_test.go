// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// The front door names everything, including the subcommands added last. It
// named ten of fifteen before the table, and a subcommand nobody can find is
// one that may as well not have been built.
func TestTheFrontDoorNamesEveryGitHubSubcommand(t *testing.T) {
	usage := usageLine()
	for _, subcommand := range githubSubcommands() {
		if !strings.Contains(usage, "github "+subcommand.Name) {
			t.Fatalf("the usage line does not name github %s", subcommand.Name)
		}
	}
}

// The GitHub command's own usage says the same thing the dispatch does,
// because it is built from the dispatch.
func TestTheGitHubUsageNamesEverySubcommand(t *testing.T) {
	usage := githubUsage()
	for _, subcommand := range githubSubcommands() {
		if !strings.Contains(usage, subcommand.Name) {
			t.Fatalf("the github usage does not name %s", subcommand.Name)
		}
	}
}

// A name typed twice is a dispatch that answers to whichever entry came
// first, and an entry with nothing behind it is a usage line that promises a
// command that does not exist.
func TestEveryGitHubSubcommandIsNamedOnceAndRunsSomething(t *testing.T) {
	seen := map[string]bool{}
	for _, subcommand := range githubSubcommands() {
		if subcommand.Name == "" {
			t.Fatal("a subcommand has no name")
		}
		if subcommand.Run == nil {
			t.Fatalf("subcommand %s runs nothing", subcommand.Name)
		}
		if seen[subcommand.Name] {
			t.Fatalf("subcommand %s is named twice", subcommand.Name)
		}
		seen[subcommand.Name] = true
	}
}

// An unknown subcommand, and a subcommand with arguments after it, are both
// answered with the usage rather than with silence or a partial run.
func TestAnUnknownGitHubSubcommandIsAnsweredWithTheUsage(t *testing.T) {
	for _, args := range [][]string{
		{"not-a-subcommand"},
		{},
		{"reconcile-page", "extra"},
	} {
		err := githubCommand(context.Background(), args)
		if err == nil || !strings.HasPrefix(err.Error(), "usage: ra8ci github ") {
			t.Fatalf("githubCommand(%q) = %v, want the usage", args, err)
		}
	}
}
