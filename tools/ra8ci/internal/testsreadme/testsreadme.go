// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package testsreadme keeps tests/README.md synchronized with the test tree.
package testsreadme

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

const (
	exitOK      = 0
	exitDrift   = 1
	exitVacuous = 2
	minSubdirs  = 5
	trustedGit  = "/usr/bin/git"
)

var rowNamePattern = regexp.MustCompile(`^\x60([A-Za-z0-9._-]+)/\x60$`)

type fixtureCase struct {
	name       string
	subdirs    []string
	documented []string
	floor      int
	wantCode   int
	needle     string
}

// Run executes the tests README check or its network-free self-test.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		if stderr != nil {
			fmt.Fprintln(stderr, "tests-readme: invalid input")
		}
		return 2
	}
	flags := flag.NewFlagSet("tests-readme", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	self := flags.Bool("selftest", false, "prove both drift directions, the floor, and gitignore behavior")
	if err := flags.Parse(args); err != nil || flags.NArg() != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci tests-readme [--selftest]")
		return 2
	}
	if *self {
		if selfTest(ctx, stdout, stderr) {
			return exitOK
		}
		return exitDrift
	}
	code, messages, count, err := evaluate(ctx, filepath.Join(root, "tests"), filepath.Join(root, "tests", "README.md"), minSubdirs, sanitizedGitEnvironment(os.Environ()))
	if err != nil {
		fmt.Fprintln(stderr, "tests-readme:", err)
		return 2
	}
	if code == exitOK {
		fmt.Fprintf(stdout, "tests/README.md OK: %d subdirectory(ies) documented, none stale\n", count)
		return exitOK
	}
	label := "drift"
	if code == exitVacuous {
		label = "collapsed scan"
	}
	fmt.Fprintf(stderr, "tests/README.md %s: %d problem(s):\n", label, len(messages))
	for _, message := range messages {
		fmt.Fprintf(stderr, "  %s\n", message)
	}
	return code
}

func evaluate(ctx context.Context, testsDir, readme string, floor int, env []string) (int, []string, int, error) {
	actual, err := immediateSubdirs(ctx, testsDir, env)
	if err != nil {
		return exitVacuous, nil, 0, err
	}
	if len(actual) < floor {
		return exitVacuous, []string{fmt.Sprintf("only %d subdirectory(ies) found under %s (floor is %d); the scan collapsed rather than the tree", len(actual), testsDir, floor)}, len(actual), nil
	}
	data, err := os.ReadFile(readme)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return exitVacuous, nil, len(actual), fmt.Errorf("read %s: %w", readme, err)
	}
	problems := driftProblems(actual, documentedSubdirs(string(data)))
	if len(problems) != 0 {
		return exitDrift, problems, len(actual), nil
	}
	return exitOK, nil, len(actual), nil
}

func immediateSubdirs(ctx context.Context, testsDir string, env []string) (map[string]struct{}, error) {
	entries, err := os.ReadDir(testsDir)
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", testsDir, err)
	}
	found := make(map[string]struct{})
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		info, err := os.Stat(filepath.Join(testsDir, name))
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return nil, fmt.Errorf("stat %s: %w", filepath.Join(testsDir, name), err)
		}
		if info.IsDir() {
			found[name] = struct{}{}
		}
	}
	ignored, err := ignoredNames(ctx, testsDir, found, env)
	if err != nil {
		return nil, err
	}
	for name := range ignored {
		delete(found, name)
	}
	return found, nil
}

func ignoredNames(ctx context.Context, testsDir string, names map[string]struct{}, env []string) (map[string]struct{}, error) {
	ignored := make(map[string]struct{})
	if len(names) == 0 {
		return ignored, nil
	}
	git, err := trustedGitExecutable()
	if err != nil {
		return nil, err
	}
	ordered := sortedNames(names)
	command := exec.CommandContext(ctx, git, "-C", testsDir, "check-ignore", "--stdin")
	command.Env = env
	command.Stdin = strings.NewReader(strings.Join(ordered, "\n") + "\n")
	var stdout bytes.Buffer
	command.Stdout = &stdout
	// No Git repository and no matching ignore rules both mean empty stdout.
	_ = command.Run()
	if ctxErr := ctx.Err(); ctxErr != nil {
		return nil, ctxErr
	}
	scanner := bufio.NewScanner(&stdout)
	for scanner.Scan() {
		if name := strings.TrimSpace(scanner.Text()); name != "" {
			ignored[name] = struct{}{}
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("read git check-ignore output: %w", err)
	}
	return ignored, nil
}

func trustedGitExecutable() (string, error) {
	info, err := os.Lstat(trustedGit)
	if err != nil {
		return "", fmt.Errorf("trusted %s is unavailable: %w", trustedGit, err)
	}
	if !info.Mode().IsRegular() || info.Mode()&0111 == 0 {
		return "", fmt.Errorf("trusted Git %s is not a regular executable", trustedGit)
	}
	return trustedGit, nil
}

func sanitizedGitEnvironment(source []string) []string {
	values := make(map[string]string, len(source)+20)
	for _, item := range source {
		name, value, ok := strings.Cut(item, "=")
		if !ok || strings.HasPrefix(name, "GIT_") || strings.HasPrefix(name, "BASH_FUNC_") {
			continue
		}
		switch name {
		case "DIFF", "EDITOR", "LESS", "LV", "MERGE_TOOL", "PAGER", "SSH_ASKPASS", "SUDO_ASKPASS", "VISUAL", "BASH_ENV", "ENV", "PYTHONHOME", "PYTHONPATH", "GH_TOKEN", "GITHUB_TOKEN", "ACTIONS_RUNTIME_TOKEN", "ACTIONS_ID_TOKEN_REQUEST_TOKEN":
			continue
		}
		values[name] = value
	}
	values["GIT_ATTR_NOSYSTEM"] = "1"
	values["GIT_CONFIG_GLOBAL"] = os.DevNull
	values["GIT_CONFIG_NOSYSTEM"] = "1"
	values["GIT_CONFIG_SYSTEM"] = os.DevNull
	values["GIT_EDITOR"] = "false"
	values["GIT_OPTIONAL_LOCKS"] = "0"
	values["GIT_PAGER"] = "cat"
	values["GIT_SEQUENCE_EDITOR"] = "false"
	values["GIT_SSH_COMMAND"] = "false"
	values["GIT_TERMINAL_PROMPT"] = "0"
	values["GIT_CONFIG_COUNT"] = "3"
	values["GIT_CONFIG_KEY_0"], values["GIT_CONFIG_VALUE_0"] = "core.hooksPath", os.DevNull
	values["GIT_CONFIG_KEY_1"], values["GIT_CONFIG_VALUE_1"] = "core.fsmonitor", "false"
	values["GIT_CONFIG_KEY_2"], values["GIT_CONFIG_VALUE_2"] = "core.attributesFile", os.DevNull
	values["PAGER"] = "cat"
	values["TERM"] = "dumb"
	result := make([]string, 0, len(values))
	for name, value := range values {
		result = append(result, name+"="+value)
	}
	sort.Strings(result)
	return result
}

func documentedSubdirs(readme string) map[string]struct{} {
	names := make(map[string]struct{})
	for _, line := range strings.Split(readme, "\n") {
		stripped := strings.TrimSpace(line)
		if !strings.HasPrefix(stripped, "|") {
			continue
		}
		cells := strings.SplitN(strings.Trim(stripped, "|"), "|", 2)
		if len(cells) != 2 {
			continue
		}
		match := rowNamePattern.FindStringSubmatch(strings.TrimSpace(cells[0]))
		if len(match) == 2 {
			names[match[1]] = struct{}{}
		}
	}
	return names
}

func driftProblems(actual, documented map[string]struct{}) []string {
	var problems []string
	for _, name := range sortedNames(actual) {
		if _, ok := documented[name]; !ok {
			problems = append(problems, fmt.Sprintf("tests/%s/ exists but is not documented in tests/README.md -- add a table row whose first cell is `%s/`", name, name))
		}
	}
	for _, name := range sortedNames(documented) {
		if _, ok := actual[name]; !ok {
			problems = append(problems, fmt.Sprintf("tests/README.md documents tests/%s/ but no such subdirectory exists -- remove or rename that row", name))
		}
	}
	return problems
}

func sortedNames(names map[string]struct{}) []string {
	result := make([]string, 0, len(names))
	for name := range names {
		result = append(result, name)
	}
	sort.Strings(result)
	return result
}

func selfTest(ctx context.Context, stdout, stderr io.Writer) bool {
	cases := []fixtureCase{
		{name: "in-sync stays quiet", subdirs: []string{"alpha", "beta", "gamma"}, documented: []string{"alpha", "beta", "gamma"}, floor: 3, wantCode: exitOK},
		{name: "undocumented subdir fires", subdirs: []string{"alpha", "beta", "gamma"}, documented: []string{"alpha", "beta"}, floor: 3, wantCode: exitDrift, needle: "tests/gamma/"},
		{name: "stale doc entry fires", subdirs: []string{"alpha", "beta", "gamma"}, documented: []string{"alpha", "beta", "gamma", "ghost"}, floor: 3, wantCode: exitDrift, needle: "ghost"},
		{name: "collapsed scan is vacuous", subdirs: []string{"alpha"}, documented: []string{"alpha"}, floor: 3, wantCode: exitVacuous},
	}
	var failures []string
	for _, test := range cases {
		if err := ctx.Err(); err != nil {
			failures = append(failures, "selftest cancelled: "+err.Error())
			break
		}
		root, err := os.MkdirTemp("", "ra8ci-tests-readme-")
		if err != nil {
			failures = append(failures, "create selftest fixture: "+err.Error())
			break
		}
		testsDir, readme, err := writeFixture(root, test)
		if err == nil {
			code, messages, _, evaluateErr := evaluate(ctx, testsDir, readme, test.floor, sanitizedGitEnvironment(os.Environ()))
			err = evaluateErr
			if err == nil && code != test.wantCode {
				failures = append(failures, fmt.Sprintf("%s: exit %d, expected %d", test.name, code, test.wantCode))
			} else if err == nil && test.needle == "" && test.wantCode == exitOK && len(messages) != 0 {
				failures = append(failures, fmt.Sprintf("%s: expected no messages, got %v", test.name, messages))
			} else if err == nil && test.needle != "" && !containsMessage(messages, test.needle) {
				failures = append(failures, fmt.Sprintf("%s: no message contains %q: %v", test.name, test.needle, messages))
			}
		}
		if err != nil {
			failures = append(failures, fmt.Sprintf("%s: %v", test.name, err))
		}
		_ = os.RemoveAll(root)
	}
	if err := selfTestGitignore(ctx); err != nil {
		failures = append(failures, err.Error())
	}
	if len(failures) != 0 {
		fmt.Fprintln(stderr, "tests-readme selftest FAILED:")
		for _, failure := range failures {
			fmt.Fprintln(stderr, failure)
		}
		return false
	}
	fmt.Fprintf(stdout, "selftest OK: %d cases plus the gitignore carve-out (both drift directions + non-vacuity floor)\n", len(cases))
	return true
}

func writeFixture(root string, test fixtureCase) (string, string, error) {
	testsDir := filepath.Join(root, "tests")
	if err := os.Mkdir(testsDir, 0755); err != nil {
		return "", "", err
	}
	for _, name := range test.subdirs {
		if err := os.Mkdir(filepath.Join(testsDir, name), 0755); err != nil {
			return "", "", err
		}
	}
	var rows strings.Builder
	for _, name := range test.documented {
		fmt.Fprintf(&rows, "| `%s/` | fixture description |\n", name)
	}
	readme := filepath.Join(testsDir, "README.md")
	data := "# tests/\n\n| Subdirectory | What it holds |\n|---|---|\n" + rows.String()
	if err := os.WriteFile(readme, []byte(data), 0644); err != nil {
		return "", "", err
	}
	return testsDir, readme, nil
}

func selfTestGitignore(ctx context.Context) error {
	git, err := trustedGitExecutable()
	if err != nil {
		return err
	}
	root, err := os.MkdirTemp("", "ra8ci-tests-readme-git-")
	if err != nil {
		return fmt.Errorf("create gitignore fixture: %w", err)
	}
	defer os.RemoveAll(root)
	env := sanitizedGitEnvironment(os.Environ())
	init := exec.CommandContext(ctx, git, "init", "--quiet", root)
	init.Env = env
	if output, err := init.CombinedOutput(); err != nil {
		return fmt.Errorf("initialize isolated gitignore fixture: %w: %s", err, strings.TrimSpace(string(output)))
	}
	if err := os.WriteFile(filepath.Join(root, ".gitignore"), []byte("build/\n"), 0644); err != nil {
		return fmt.Errorf("write gitignore fixture: %w", err)
	}
	testsDir := filepath.Join(root, "tests")
	if err := os.Mkdir(testsDir, 0755); err != nil {
		return fmt.Errorf("create gitignore tests fixture: %w", err)
	}
	for _, name := range []string{"alpha", "beta", "gamma", "build"} {
		if err := os.Mkdir(filepath.Join(testsDir, name), 0755); err != nil {
			return fmt.Errorf("create gitignore fixture directory: %w", err)
		}
	}
	var rows strings.Builder
	for _, name := range []string{"alpha", "beta", "gamma"} {
		fmt.Fprintf(&rows, "| `%s/` | fixture |\n", name)
	}
	readme := filepath.Join(testsDir, "README.md")
	if err := os.WriteFile(readme, []byte("# tests/\n\n| Subdirectory | What it holds |\n|---|---|\n"+rows.String()), 0644); err != nil {
		return fmt.Errorf("write gitignore README fixture: %w", err)
	}
	code, messages, _, err := evaluate(ctx, testsDir, readme, 3, env)
	if err != nil || code != exitOK {
		return fmt.Errorf("ignored build/ still demanded a README row: exit %d %v (%v)", code, messages, err)
	}
	if err := os.Mkdir(filepath.Join(testsDir, "delta"), 0755); err != nil {
		return fmt.Errorf("create trackable gitignore fixture: %w", err)
	}
	code, messages, _, err = evaluate(ctx, testsDir, readme, 3, env)
	if err != nil || code != exitDrift || !containsMessage(messages, "tests/delta/") {
		return fmt.Errorf("a trackable subdir stopped firing: exit %d %v (%v)", code, messages, err)
	}
	return nil
}

func containsMessage(messages []string, needle string) bool {
	for _, message := range messages {
		if strings.Contains(message, needle) {
			return true
		}
	}
	return false
}
