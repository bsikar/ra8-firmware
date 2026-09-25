// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// The front door prints the usage line, so every command it dispatches has to
// be in it. It was written out by hand and board-agent was never in it.
func TestTheFrontDoorNamesEveryTopLevelCommand(t *testing.T) {
	usage := usageLine()
	for _, command := range topLevelCommands() {
		if !strings.Contains(usage, command.Usage) {
			t.Errorf("usage does not state %q: %s", command.Name, usage)
		}
	}
}

func TestTheFrontDoorNamesTheBoardAgent(t *testing.T) {
	if _, ok := topLevelCommandNamed("board-agent"); !ok {
		t.Fatal("board-agent is not dispatched")
	}
	if !strings.Contains(usageLine(), "board-agent") {
		t.Errorf("usage does not name board-agent: %s", usageLine())
	}
}

func TestEveryTopLevelCommandIsNamedOnceAndRunsSomething(t *testing.T) {
	seen := map[string]bool{}
	for _, command := range topLevelCommands() {
		if command.Name == "" {
			t.Error("a command has no name")
			continue
		}
		if seen[command.Name] {
			t.Errorf("%s is named twice", command.Name)
		}
		seen[command.Name] = true
		if command.Usage == "" {
			t.Errorf("%s states no usage", command.Name)
		}
		if command.Run == nil {
			t.Errorf("%s runs nothing", command.Name)
		}
	}
}

// A command stated under a name it is not dispatched under is a command
// nobody can type.
func TestEveryUsageBeginsWithTheNameItIsTypedAs(t *testing.T) {
	for _, command := range topLevelCommands() {
		if !strings.HasPrefix(command.Usage, command.Name) {
			t.Errorf("%s is stated as %q", command.Name, command.Usage)
		}
	}
}

func TestTheFrontDoorLeavesRoomForATask(t *testing.T) {
	usage := usageLine()
	if !strings.HasPrefix(usage, "usage: ra8ci <task>|") {
		t.Errorf("the front door does not open with a task: %s", usage)
	}
	if strings.Contains(usage, "\n") {
		t.Errorf("the front door is more than one line: %s", usage)
	}
}

// The GitHub half is the table it was taken from, not a second list.
func TestTheFrontDoorNamesEveryGitHubSubcommandToo(t *testing.T) {
	usage := usageLine()
	for _, subcommand := range githubSubcommands() {
		if !strings.Contains(usage, "github "+subcommand.Name) {
			t.Errorf("usage does not state github %s: %s", subcommand.Name, usage)
		}
	}
}

func TestAnUnknownCommandIsNotDispatched(t *testing.T) {
	for _, name := range []string{"", "githu", "board agent", "TASKS"} {
		if _, ok := topLevelCommandNamed(name); ok {
			t.Errorf("%q is dispatched", name)
		}
	}
}
