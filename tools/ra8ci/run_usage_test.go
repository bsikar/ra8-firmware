// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// The front door stated `run submit|run status` while runCommand dispatched
// five subcommands, so logs, events and cancel were reachable only by
// knowing they were there.
func TestTheFrontDoorNamesEveryRunSubcommand(t *testing.T) {
	usage := usageLine()
	for _, subcommand := range runSubcommands() {
		if !strings.Contains(usage, subcommand.Name) {
			t.Errorf("the front door does not state run %s: %s", subcommand.Name, usage)
		}
	}
}

func TestTheRunUsageNamesEverySubcommand(t *testing.T) {
	usage := runUsage()
	if !strings.HasPrefix(usage, "run ") {
		t.Fatalf("the run usage is not typed as run: %s", usage)
	}
	for _, subcommand := range runSubcommands() {
		if !strings.Contains(usage, subcommand.Name) {
			t.Errorf("the run usage does not state %s: %s", subcommand.Name, usage)
		}
	}
}

func TestEveryRunSubcommandIsNamedOnceAndRunsSomething(t *testing.T) {
	seen := map[string]bool{}
	for _, subcommand := range runSubcommands() {
		if subcommand.Name == "" {
			t.Error("a run subcommand has no name")
			continue
		}
		if seen[subcommand.Name] {
			t.Errorf("run %s is named twice", subcommand.Name)
		}
		seen[subcommand.Name] = true
		if subcommand.Run == nil {
			t.Errorf("run %s runs nothing", subcommand.Name)
		}
	}
}

// An unknown subcommand is answered with the usage, and nothing is run.
func TestAnUnknownRunSubcommandIsAnsweredWithTheUsage(t *testing.T) {
	for _, args := range [][]string{nil, {}, {"resubmit"}, {"Submit"}, {""}} {
		err := runCommand(context.Background(), args)
		if err == nil {
			t.Fatalf("%v was accepted", args)
		}
		if !strings.HasPrefix(err.Error(), "usage: ra8ci run ") {
			t.Errorf("%v answered with %q", args, err)
		}
	}
}

// The command and the front door read one list, so they cannot disagree.
func TestTheFrontDoorAndTheCommandStateTheSameRunUsage(t *testing.T) {
	command, ok := topLevelCommandNamed("run")
	if !ok {
		t.Fatal("run is not dispatched")
	}
	if command.Usage != runUsage() {
		t.Errorf("the front door states %q, the command states %q", command.Usage, runUsage())
	}
	if err := runCommand(context.Background(), nil); err == nil || !strings.Contains(err.Error(), command.Usage) {
		t.Errorf("the command's usage does not carry the front door's: %v", err)
	}
}
