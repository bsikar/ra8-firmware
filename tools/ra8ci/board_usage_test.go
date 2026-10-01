// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// The front door stated `board status|take|checkpoint|extend|cancel` while
// boardCommand dispatched eight subcommands, so heartbeat, liveness and
// recover were reachable only by knowing they were there.
func TestTheFrontDoorNamesEveryBoardSubcommand(t *testing.T) {
	usage := usageLine()
	for _, subcommand := range boardSubcommands() {
		if !strings.Contains(usage, subcommand.Name) {
			t.Errorf("the front door does not state board %s: %s", subcommand.Name, usage)
		}
	}
}

func TestTheBoardUsageNamesEverySubcommand(t *testing.T) {
	usage := boardUsage()
	if !strings.HasPrefix(usage, "board ") {
		t.Fatalf("the board usage is not typed as board: %s", usage)
	}
	for _, subcommand := range boardSubcommands() {
		if !strings.Contains(usage, subcommand.Name) {
			t.Errorf("the board usage does not state %s: %s", subcommand.Name, usage)
		}
	}
}

// The long answer states every subcommand with its arguments, so an operator
// who typed a name nothing dispatches is told what there is to type.
func TestTheBoardUsageErrorStatesEverySubcommandsArguments(t *testing.T) {
	err := boardUsageError()
	if err == nil {
		t.Fatal("the board usage is not an error")
	}
	if !strings.HasPrefix(err.Error(), "usage: ra8ci board ") {
		t.Fatalf("the board usage error is not typed as board: %v", err)
	}
	for _, subcommand := range boardSubcommands() {
		if !strings.Contains(err.Error(), "board "+subcommand.Usage) {
			t.Errorf("the board usage error does not state %q: %v", subcommand.Usage, err)
		}
	}
}

func TestEveryBoardSubcommandIsNamedOnceAndRunsSomething(t *testing.T) {
	seen := map[string]bool{}
	for _, subcommand := range boardSubcommands() {
		if subcommand.Name == "" {
			t.Error("a board subcommand has no name")
			continue
		}
		if seen[subcommand.Name] {
			t.Errorf("board %s is named twice", subcommand.Name)
		}
		seen[subcommand.Name] = true
		if subcommand.Run == nil {
			t.Errorf("board %s runs nothing", subcommand.Name)
		}
		if !strings.HasPrefix(subcommand.Usage, subcommand.Name) {
			t.Errorf("board %s is stated as %q", subcommand.Name, subcommand.Usage)
		}
	}
}

// A name nothing dispatches, and a status or take that is not a request, are
// answered with the usage; nothing is run and no client is built.
func TestAnUnknownBoardSubcommandIsAnsweredWithTheUsage(t *testing.T) {
	for _, args := range [][]string{nil, {}, {"bogus"}, {"Status"}, {""}, {"status"}, {"status", "a", "b"}, {"take"}} {
		err := boardCommand(context.Background(), args)
		if err == nil {
			t.Fatalf("%v was accepted", args)
		}
		if !strings.HasPrefix(err.Error(), "usage: ra8ci board ") {
			t.Errorf("%v answered with %q", args, err)
		}
	}
}

// A subcommand that states its own arguments keeps stating them: checkpoint
// answers for itself rather than with the whole board usage.
func TestCheckpointAnswersWithItsOwnUsage(t *testing.T) {
	err := boardCommand(context.Background(), []string{"checkpoint"})
	if err == nil {
		t.Fatal("checkpoint with no board was accepted")
	}
	if err.Error() != "usage: ra8ci board checkpoint <board-id>" {
		t.Errorf("checkpoint answered with %q", err)
	}
}

// The command and the front door read one list, so they cannot disagree.
func TestTheFrontDoorAndTheCommandStateTheSameBoardUsage(t *testing.T) {
	command, ok := topLevelCommandNamed("board")
	if !ok {
		t.Fatal("board is not dispatched")
	}
	if command.Usage != boardUsage() {
		t.Errorf("the front door states %q, the command states %q", command.Usage, boardUsage())
	}
}
