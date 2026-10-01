// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// privateConfigHome points the lease store at a temporary configuration home,
// so a board command that reads the local lease directory before it reaches
// for credentials does not touch the real one.
func privateConfigHome(t *testing.T) string {
	t.Helper()
	home := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", home)
	return home
}

func TestBoardRecoverRefusesEveryInvocationThatApprovesNoPlan(t *testing.T) {
	noServerNamed(t)
	usage := "usage: ra8ci board recover <board-id> --plan <plan-id> --why <reason>"
	plan := soundRunID(t)
	cases := map[string]struct {
		args []string
		want string
	}{
		"no board":                     {nil, usage},
		"a board name that is not one": {[]string{"ek ra8d2", "--plan", plan, "--why", "bricked"}, usage},
		"a flag that is not one":       {[]string{"ek-ra8d2", "--force", "--plan", plan, "--why", "bricked"}, usage},
		"a positional left over":       {[]string{"ek-ra8d2", "--plan", plan, "--why", "bricked", "now"}, usage},
		"no reason":                    {[]string{"ek-ra8d2", "--plan", plan}, usage},
		"an empty reason":              {[]string{"ek-ra8d2", "--plan", plan, "--why", ""}, usage},
		"no plan at all": {[]string{"ek-ra8d2", "--why", "bricked"},
			"board recover --plan must be the identifier of a reviewed recovery plan"},
		"a plan that is not an identifier": {[]string{"ek-ra8d2", "--plan", "plan-1", "--why", "bricked"},
			"board recover --plan must be the identifier of a reviewed recovery plan"},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			err := boardRecoverCommand(context.Background(), testCase.args)
			if err == nil || !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("err=%v; want it to carry %q", err, testCase.want)
			}
			// A recovery sequence is hardware moving under an operator's
			// approval, so no refusal here may have got as far as the server.
			if strings.Contains(err.Error(), envClientCert) {
				t.Fatalf("err=%v; want the refusal taken before any credential is read", err)
			}
		})
	}
}

func TestBoardRecoverWithAReviewedPlanStopsAtTheEnvironment(t *testing.T) {
	noServerNamed(t)
	err := boardRecoverCommand(context.Background(),
		[]string{"ek-ra8d2", "--plan", soundRunID(t), "--why", "board wedged after a failed flash"})
	if err == nil {
		t.Fatal("board recover reached out with no server named")
	}
	if !strings.Contains(err.Error(), "ra8ci board") || !strings.Contains(err.Error(), envClientCert) {
		t.Fatalf("err=%v; want the credential refusal naming the board command", err)
	}
}

func TestBoardHeartbeatAndLivenessRefuseABoardNameThatIsNotOne(t *testing.T) {
	noServerNamed(t)
	privateConfigHome(t)
	commands := map[string]struct {
		run   func(context.Context, []string) error
		usage string
	}{
		"heartbeat":  {boardHeartbeatCommand, "usage: ra8ci board heartbeat <board-id>"},
		"liveness":   {boardLivenessCommand, "usage: ra8ci board liveness <board-id>"},
		"checkpoint": {boardCheckpointCommand, "usage: ra8ci board checkpoint <board-id>"},
	}
	spoiled := map[string][]string{
		"no board":            nil,
		"two boards":          {"ek-ra8d2", "ek-ra8m1"},
		"a space in the name": {"ek ra8d2"},
		"a path in the name":  {"../ek-ra8d2"},
		"an empty name":       {""},
		"a name too long":     {strings.Repeat("b", 129)},
	}
	for command, subject := range commands {
		for name, args := range spoiled {
			t.Run(command+" with "+name, func(t *testing.T) {
				err := subject.run(context.Background(), args)
				if err == nil || err.Error() != subject.usage {
					t.Fatalf("err=%v; want exactly %q", err, subject.usage)
				}
			})
		}
	}
}

func TestBoardHeartbeatReadsItsOwnLeaseStoreBeforeReachingForCredentials(t *testing.T) {
	noServerNamed(t)
	home := privateConfigHome(t)
	err := boardHeartbeatCommand(context.Background(), []string{"ek-ra8d2"})
	if err == nil {
		t.Fatal("board heartbeat reached out with no server named")
	}
	if !strings.Contains(err.Error(), envClientCert) {
		t.Fatalf("err=%v; want the credential refusal", err)
	}
	// A beat is made from the token this machine already holds, so the local
	// store is consulted first and the private directory exists by the time
	// credentials are read. Liveness is a read of the server's own view and
	// never touches it.
	info, statErr := os.Lstat(filepath.Join(home, "ra8ci"))
	if statErr != nil || !info.IsDir() || info.Mode().Perm() != 0o700 {
		t.Fatalf("configuration directory info=%v err=%v; want a 0700 directory created by the beat", info, statErr)
	}
}

func TestBoardLivenessLeavesNoLeaseStoreBehind(t *testing.T) {
	noServerNamed(t)
	home := privateConfigHome(t)
	if err := boardLivenessCommand(context.Background(), []string{"ek-ra8d2"}); err == nil ||
		!strings.Contains(err.Error(), envClientCert) {
		t.Fatalf("err=%v; want the credential refusal", err)
	}
	if _, err := os.Lstat(filepath.Join(home, "ra8ci")); err == nil {
		t.Fatal("a liveness read created the local lease store; it is a read of the server's view")
	}
}

func TestBoardExtendRefusesADurationItWouldNotHoldTo(t *testing.T) {
	noServerNamed(t)
	privateConfigHome(t)
	usage := "usage: ra8ci board extend <board-id> --why <reason> --duration <duration>"
	bounds := "board extension duration must be a whole number of seconds between 1s and 8h"
	cases := map[string]struct {
		args []string
		want string
	}{
		"no board":                     {nil, usage},
		"a board name that is not one": {[]string{"ek ra8d2", "--why", "still debugging", "--duration", "30s"}, usage},
		"a flag that is not one":       {[]string{"ek-ra8d2", "--forever", "--why", "w", "--duration", "30s"}, usage},
		"a positional left over":       {[]string{"ek-ra8d2", "--why", "w", "--duration", "30s", "please"}, usage},
		"no reason":                    {[]string{"ek-ra8d2", "--duration", "30s"}, usage},
		"no duration":                  {[]string{"ek-ra8d2", "--why", "still debugging"}, usage},
		"a duration that is not one":   {[]string{"ek-ra8d2", "--why", "w", "--duration", "soon"}, bounds},
		"no time at all":               {[]string{"ek-ra8d2", "--why", "w", "--duration", "0s"}, bounds},
		"time going backwards":         {[]string{"ek-ra8d2", "--why", "w", "--duration", "-30s"}, bounds},
		"part of a second":             {[]string{"ek-ra8d2", "--why", "w", "--duration", "1500ms"}, bounds},
		"past the eight-hour ceiling":  {[]string{"ek-ra8d2", "--why", "w", "--duration", "8h1s"}, bounds},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			err := boardExtendCommand(context.Background(), testCase.args)
			if err == nil || !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("err=%v; want it to carry %q", err, testCase.want)
			}
			if strings.Contains(err.Error(), envClientCert) {
				t.Fatalf("err=%v; want the refusal taken before any credential is read", err)
			}
		})
	}
	// Exactly eight hours is the ceiling, not past it, so it gets as far as
	// the credentials the command would need.
	if err := boardExtendCommand(context.Background(),
		[]string{"ek-ra8d2", "--why", "long bring-up", "--duration", "8h"}); err == nil ||
		!strings.Contains(err.Error(), envClientCert) {
		t.Fatalf("err=%v; want the ceiling itself accepted and the command stopped at its credentials", err)
	}
}

func TestBoardStatusRefusesAnythingButOneBoard(t *testing.T) {
	noServerNamed(t)
	for name, args := range map[string][]string{
		"no board":   nil,
		"two boards": {"ek-ra8d2", "ek-ra8m1"},
	} {
		t.Run(name, func(t *testing.T) {
			err := boardStatusCommand(context.Background(), args)
			if err == nil || !strings.Contains(err.Error(), "usage: ra8ci board") {
				t.Fatalf("err=%v; want the board usage", err)
			}
			// The shared usage names every subcommand, so an operator who got
			// the arity wrong is shown the whole grammar rather than one line.
			for _, subcommand := range boardSubcommands() {
				if !strings.Contains(err.Error(), subcommand.Name) {
					t.Fatalf("err=%v; want it to name %q", err, subcommand.Name)
				}
			}
		})
	}
}
