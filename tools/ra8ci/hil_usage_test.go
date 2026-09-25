// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

func TestTheFrontDoorNamesEveryHILSubcommand(t *testing.T) {
	usage := usageLine()
	for _, subcommand := range hilSubcommands() {
		if !strings.Contains(usage, subcommand.Name) {
			t.Errorf("the front door does not state hil %s: %s", subcommand.Name, usage)
		}
	}
}

func TestTheHILUsageNamesEverySubcommand(t *testing.T) {
	usage := hilUsage()
	if !strings.HasPrefix(usage, "hil ") {
		t.Fatalf("the HIL usage is not typed as hil: %s", usage)
	}
	for _, subcommand := range hilSubcommands() {
		if !strings.Contains(usage, subcommand.Name) {
			t.Errorf("the HIL usage does not state %s: %s", subcommand.Name, usage)
		}
	}
}

// The gap this slice closes: `ra8ci hil bogus` was answered with the budget
// usage alone, so an operator who mistyped was never told verify-capture
// exists, and `ra8ci hil` with nothing at all stated budget as "budget ...".
func TestAnUnknownHILSubcommandIsAnsweredWithEverySubcommand(t *testing.T) {
	for _, args := range [][]string{nil, {}, {"bogus"}, {"Budget"}, {""}, {"verify"}} {
		err := hilCommand(context.Background(), args)
		if err == nil {
			t.Fatalf("%v was accepted", args)
		}
		if !strings.HasPrefix(err.Error(), "usage: ra8ci hil ") {
			t.Fatalf("%v answered with %q", args, err)
		}
		for _, subcommand := range hilSubcommands() {
			if !strings.Contains(err.Error(), "hil "+subcommand.Usage) {
				t.Errorf("%v was not told about hil %s: %v", args, subcommand.Name, err)
			}
		}
	}
}

func TestEveryHILSubcommandIsNamedOnceAndRunsSomething(t *testing.T) {
	seen := map[string]bool{}
	for _, subcommand := range hilSubcommands() {
		if subcommand.Name == "" {
			t.Error("a HIL subcommand has no name")
			continue
		}
		if seen[subcommand.Name] {
			t.Errorf("hil %s is named twice", subcommand.Name)
		}
		seen[subcommand.Name] = true
		if subcommand.Run == nil {
			t.Errorf("hil %s runs nothing", subcommand.Name)
		}
		if !strings.HasPrefix(subcommand.Usage, subcommand.Name) {
			t.Errorf("hil %s is stated as %q", subcommand.Name, subcommand.Usage)
		}
	}
}

// A subcommand that reaches its own flags keeps answering for itself.
func TestBudgetWithoutItsFlagsAnswersWithTheBudgetUsage(t *testing.T) {
	err := hilCommand(context.Background(), []string{"budget"})
	if err == nil {
		t.Fatal("budget with no flags was accepted")
	}
	if strings.Contains(err.Error(), "verify-capture") {
		t.Errorf("budget answered with the whole HIL usage: %v", err)
	}
}

func TestTheFrontDoorAndTheCommandStateTheSameHILUsage(t *testing.T) {
	command, ok := topLevelCommandNamed("hil")
	if !ok {
		t.Fatal("hil is not dispatched")
	}
	if command.Usage != hilUsage() {
		t.Errorf("the front door states %q, the command states %q", command.Usage, hilUsage())
	}
}
