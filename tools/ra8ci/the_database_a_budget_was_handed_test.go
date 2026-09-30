// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
	"time"
)

// The budget command's refusals are a ladder, and the cases in
// the_hil_commands_refuse_before_they_measure_test.go take the rungs above
// this one: a spoiled flag, a manifest that will not load, an absent
// RA8CI_DATABASE_URL, and an absent context. Each stops the command before
// the one below it.
//
// This is the last rung reachable without Postgres: a database that is named
// and cannot be opened. It matters on its own because an operator reading
// "ra8ci hil budget requires RA8CI_DATABASE_URL" goes and sets the variable,
// and an operator reading "open HIL timing database" has already set it and
// needs to know the value is the problem. Conflating the two would send them
// round the loop.
func TestHILBudgetReportsADatabaseItWasGivenAndCannotOpen(t *testing.T) {
	for name, dsn := range map[string]string{
		"not a connection string at all": "this is not a dsn",
		"a scheme nothing serves":        "carrier-pigeon://ra8ci/timings",
		"a port that is not a number":    "postgres://ra8ci@127.0.0.1:not-a-port/ra8ci",
	} {
		t.Run(name, func(t *testing.T) {
			t.Setenv("RA8CI_DATABASE_URL", dsn)
			finished := make(chan error, 1)
			go func() { finished <- hilBudgetCommand(context.Background(), soundBudgetArguments()) }()

			var err error
			select {
			case err = <-finished:
			case <-time.After(10 * time.Second):
				t.Fatalf("the command did not answer; a DSN this broken must be refused, never dialled")
			}
			if err == nil {
				t.Fatal("an unopenable timing database was accepted")
			}
			if !strings.HasPrefix(err.Error(), "open HIL timing database:") {
				t.Fatalf("refusal = %q, want it to name the open and carry the reason", err)
			}
			// The variable was set, so the command must not fall back to
			// telling the operator to set it.
			if strings.Contains(err.Error(), "requires RA8CI_DATABASE_URL") {
				t.Fatalf("refusal = %q, want the named database reported rather than an absent one", err)
			}
		})
	}
}

// A DSN that is only whitespace is not a named database. The command trims
// before it judges, so this belongs with the absent variable rather than with
// the unopenable ones above, and an operator who exported an empty string is
// told to set it rather than told it could not be opened.
func TestHILBudgetTreatsAWhitespaceDatabaseAsNoneAtAll(t *testing.T) {
	for name, dsn := range map[string]string{
		"a single space": " ",
		"a tab":          "\t",
		"a newline":      "\n",
		"several":        "  \t\n ",
	} {
		t.Run(name, func(t *testing.T) {
			t.Setenv("RA8CI_DATABASE_URL", dsn)
			err := hilBudgetCommand(context.Background(), soundBudgetArguments())
			if err == nil {
				t.Fatal("a whitespace database was accepted")
			}
			if err.Error() != "ra8ci hil budget requires RA8CI_DATABASE_URL" {
				t.Fatalf("refusal = %q, want the absent database rather than an open failure", err)
			}
		})
	}
}
