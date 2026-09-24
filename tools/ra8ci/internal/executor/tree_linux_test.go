//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestParseProcStatPositionsFieldsFromTheLastParenthesis(t *testing.T) {
	cases := []struct {
		name string
		line string
		want treeProcess
		ok   bool
	}{
		{
			name: "ordinary command",
			line: "4242 (sleep) S 4200 4100 4100 0 -1 4194304 " +
				strings.Repeat("0 ", 12) + "981234 0 0 0 0 0",
			want: treeProcess{PID: 4242, PPID: 4200, PGID: 4100, StartTicks: 981234},
			ok:   true,
		},
		{
			name: "command containing spaces and parentheses",
			line: "77 (weird ) name (x)) S 7 5 5 0 -1 0 " +
				strings.Repeat("0 ", 12) + "42 0 0",
			want: treeProcess{PID: 77, PPID: 7, PGID: 5, StartTicks: 42},
			ok:   true,
		},
		{name: "no closing parenthesis", line: "1 sleep S 0 0 0", ok: false},
		{name: "truncated before starttime", line: "1 (sleep) S 0 0 0", ok: false},
		{name: "empty", line: "", ok: false},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			got, ok := parseProcStat(testCase.want.PID, []byte(testCase.line))
			if ok != testCase.ok {
				t.Fatalf("ok = %v, want %v", ok, testCase.ok)
			}
			if ok && got != testCase.want {
				t.Fatalf("parsed = %+v, want %+v", got, testCase.want)
			}
		})
	}
}

func TestParseProcStatRefusesNegativeIdentity(t *testing.T) {
	line := "5 (sh) S -1 5 5 0 -1 0 " + strings.Repeat("0 ", 12) + "7 0 0"
	if _, ok := parseProcStat(5, []byte(line)); ok {
		t.Fatal("a negative parent id was accepted")
	}
	if _, ok := parseProcStat(0, []byte("0 (sh) S 1 1 1 0 -1 0 "+strings.Repeat("0 ", 12)+"7 0 0")); ok {
		t.Fatal("pid 0 was accepted")
	}
}

func TestEscapedDescendantsKeepsOnlyDescendantsOutsideTheGroup(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 10},  // the step leader
		11: {PID: 11, PPID: 10, PGID: 10}, // ordinary child, group covers it
		12: {PID: 12, PPID: 11, PGID: 10}, // ordinary grandchild
		13: {PID: 13, PPID: 10, PGID: 13}, // called setsid, escaped
		14: {PID: 14, PPID: 13, PGID: 13}, // child of the escapee
		20: {PID: 20, PPID: 1, PGID: 20},  // unrelated
		21: {PID: 21, PPID: 20, PGID: 21}, // unrelated and outside its group
	}
	escaped := escapedDescendants(10, table)
	found := map[int]bool{}
	for _, process := range escaped {
		found[process.PID] = true
	}
	if len(escaped) != 2 || !found[13] || !found[14] {
		t.Fatalf("escaped = %+v, want exactly 13 and 14", escaped)
	}
	if got := escapedDescendants(1, table); got != nil {
		t.Fatalf("pid 1 walked: %+v", got)
	}
	if got := escapedDescendants(10, nil); got != nil {
		t.Fatalf("empty table walked: %+v", got)
	}
	if got := escapedDescendants(99, table); got != nil {
		t.Fatalf("unknown leader walked: %+v", got)
	}
}

func TestEscapedDescendantsSurvivesACycleAndSelfParent(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 10},
		11: {PID: 11, PPID: 10, PGID: 11},
		12: {PID: 12, PPID: 11, PGID: 12},
		13: {PID: 13, PPID: 13, PGID: 13},
	}
	table[11] = treeProcess{PID: 11, PPID: 12, PGID: 11}
	done := make(chan []treeProcess, 1)
	go func() { done <- escapedDescendants(10, table) }()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("the walk did not terminate on a parent cycle")
	}
}

// startSleeper runs a detached sleeper the test owns, so the identity checks
// can be exercised against a real live process.
func startSleeper(t *testing.T) *exec.Cmd {
	t.Helper()
	sleeper := exec.Command("/bin/sleep", "30")
	if err := sleeper.Start(); err != nil {
		t.Fatalf("start sleeper: %v", err)
	}
	t.Cleanup(func() {
		_ = sleeper.Process.Kill()
		_, _ = sleeper.Process.Wait()
	})
	return sleeper
}

func TestSignalEscapedDescendantsRefusesARecycledPID(t *testing.T) {
	sleeper := startSleeper(t)
	live, ok := readProcess(sleeper.Process.Pid)
	if !ok {
		t.Fatal("the sleeper was not readable through /proc")
	}
	stale := live
	stale.StartTicks = live.StartTicks + 1
	delivered, err := signalEscapedDescendants([]treeProcess{stale}, syscall.SIGKILL)
	if err != nil || delivered != 0 {
		t.Fatalf("delivered = %d, error = %v, want a refused signal", delivered, err)
	}
	if _, stillThere := readProcess(sleeper.Process.Pid); !stillThere {
		t.Fatal("a process whose start time did not match was killed")
	}
}

func TestSignalEscapedDescendantsNeverTouchesInitOrItself(t *testing.T) {
	self, ok := readProcess(os.Getpid())
	if !ok {
		t.Fatal("this test process was not readable through /proc")
	}
	delivered, err := signalEscapedDescendants([]treeProcess{
		{PID: 1, PPID: 0, PGID: 1, StartTicks: 0},
		self,
	}, syscall.SIGKILL)
	if err != nil || delivered != 0 {
		t.Fatalf("delivered = %d, error = %v, want nothing signalled", delivered, err)
	}
}

func TestSignalEscapedDescendantsKillsALiveMatch(t *testing.T) {
	sleeper := startSleeper(t)
	live, ok := readProcess(sleeper.Process.Pid)
	if !ok {
		t.Fatal("the sleeper was not readable through /proc")
	}
	delivered, err := signalEscapedDescendants([]treeProcess{live}, syscall.SIGKILL)
	if err != nil || delivered != 1 {
		t.Fatalf("delivered = %d, error = %v, want exactly one signal", delivered, err)
	}
	if _, waitErr := sleeper.Process.Wait(); waitErr != nil {
		t.Fatalf("wait killed sleeper: %v", waitErr)
	}
}

// waitForGone polls until the pid is gone or a zombie, the same evidence the
// existing process-group cancellation test accepts.
func waitForGone(t *testing.T, pid int, limit time.Duration) bool {
	t.Helper()
	deadline := time.Now().Add(limit)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "stat"))
		if os.IsNotExist(err) {
			return true
		}
		if err == nil {
			if fields := strings.Fields(string(data)); len(fields) > 2 && fields[2] == "Z" {
				return true
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	return false
}

func TestCancellationTerminatesADescendantThatLeftTheProcessGroup(t *testing.T) {
	if _, err := os.Stat("/usr/bin/setsid"); err != nil {
		t.Skipf("setsid is not present on this host: %v", err)
	}
	root := t.TempDir()
	pidFile := filepath.Join(root, "escapee.pid")
	// The inner shell puts itself in a new session, so the leader's process
	// group signal cannot reach it, while it stays a child in the tree.
	script := "/usr/bin/setsid /bin/sh -c 'echo $$ > " + pidFile + "; exec /bin/sleep 60' & /bin/sleep 60"
	// Real files, not pipes: a pipe the escapee inherits would keep the
	// parent's output copy open and measure WaitDelay rather than the
	// teardown this test is about.
	output, err := os.OpenFile(os.DevNull, os.O_WRONLY, 0)
	if err != nil {
		t.Fatalf("open null sink: %v", err)
	}
	t.Cleanup(func() { _ = output.Close() })
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	type outcome struct {
		result commandResult
		err    error
	}
	finished := make(chan outcome, 1)
	go func() {
		result, err := runCommand(ctx, "/bin/sh", []string{"-c", script}, root, cleanTestEnvironment(), output, output, 50*time.Millisecond)
		finished <- outcome{result: result, err: err}
	}()
	pid := 0
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(pidFile)
		if err == nil && strings.TrimSpace(string(data)) != "" {
			pid, err = strconv.Atoi(strings.TrimSpace(string(data)))
			if err != nil {
				t.Fatalf("escapee pid: %v", err)
			}
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if pid == 0 {
		cancel()
		<-finished
		t.Skip("the escaped descendant never recorded its pid on this host")
	}
	t.Cleanup(func() { _ = syscall.Kill(pid, syscall.SIGKILL) })
	escapee, ok := readProcess(pid)
	if !ok {
		t.Skip("the escaped descendant exited before it could be observed")
	}
	leader, ok := readProcess(escapee.PPID)
	if !ok {
		t.Skip("the escapee's parent exited before it could be observed")
	}
	if escapee.PGID == leader.PGID {
		t.Fatalf("the descendant did not leave the group: pgid %d", escapee.PGID)
	}
	cancel()
	select {
	case done := <-finished:
		if done.err != nil {
			t.Fatalf("run: %v", done.err)
		}
		if !done.result.Cancelled {
			t.Fatalf("result = %+v, want cancelled", done.result)
		}
	case <-time.After(20 * time.Second):
		t.Fatal("cancellation did not finish")
	}
	if !waitForGone(t, pid, 5*time.Second) {
		t.Fatalf("escaped descendant %d survived cancellation", pid)
	}
}

func cleanTestEnvironment() []string {
	return []string{"PATH=/usr/bin:/bin", "HOME=/nonexistent"}
}
