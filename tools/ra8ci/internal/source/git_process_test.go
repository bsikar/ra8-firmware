// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package source

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

const fakeGitEnvironment = "RA8CI_SOURCE_FAKE_GIT"
const fakeGitArgsEnvironment = "RA8CI_SOURCE_FAKE_GIT_ARGS"
const theCommit = "1f2e3d4c5b6a798807162534435261708f9e0d1c"

type fakeGitResponse struct {
	Stdout []byte
	Stderr []byte
	Exit   int
}

type fakeGitFixture struct {
	Responses    map[string]fakeGitResponse
	RootTreePath string
	RootTree     []byte
}

func stubbedGit(t *testing.T, lsTree []byte) string {
	t.Helper()
	return stubbedGitResponse(t, fakeGitResponse{Stdout: lsTree})
}

func stubbedGitResponse(t *testing.T, response fakeGitResponse) string {
	t.Helper()
	return installFakeGit(t, fakeGitFixture{
		Responses: map[string]fakeGitResponse{
			"ls-tree": response,
		},
	})
}

func stubbedGitForRootTree(t *testing.T, root string, tree []byte) string {
	t.Helper()
	return installFakeGit(t, fakeGitFixture{RootTreePath: root, RootTree: tree})
}

func installFakeGit(t *testing.T, fixture fakeGitFixture) string {
	t.Helper()
	defaults := map[string]fakeGitResponse{
		"rev-parse": {Stdout: []byte(theCommit + "\n")},
		"status":    {},
		"archive":   {Stdout: []byte("tar-bytes\n")},
		"ls-tree":   {},
	}
	for command, response := range fixture.Responses {
		defaults[command] = response
	}
	fixture.Responses = defaults

	encoded, err := json.Marshal(fixture)
	if err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(t.TempDir(), "git-fixture")
	if err := os.WriteFile(marker, []byte("fixture executable identity\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	priorLookPath, priorCommand := gitLookPath, gitCommand
	gitLookPath = func(name string) (string, error) {
		if name != "git" {
			return "", exec.ErrNotFound
		}
		return marker, nil
	}
	gitCommand = func(ctx context.Context, _ string, directory string, args ...string) *exec.Cmd {
		executable, executableErr := os.Executable()
		if executableErr != nil {
			t.Fatal(executableErr)
		}
		command := exec.CommandContext(ctx, executable, "-test.run=TestFakeGitSubprocess")
		payload := base64.StdEncoding.EncodeToString(encoded)
		encodedArgs, err := json.Marshal(append([]string{"-C", directory}, args...))
		if err != nil {
			t.Fatal(err)
		}
		command.Env = []string{
			fakeGitEnvironment + "=" + payload,
			fakeGitArgsEnvironment + "=" + base64.StdEncoding.EncodeToString(encodedArgs),
		}
		if runtime.GOOS == "windows" {
			command.Env = append(command.Env,
				"SYSTEMROOT="+os.Getenv("SYSTEMROOT"),
				"TEMP="+os.Getenv("TEMP"),
			)
		}
		return command
	}
	t.Cleanup(func() {
		gitLookPath, gitCommand = priorLookPath, priorCommand
	})
	return marker
}

func TestFakeGitSubprocess(t *testing.T) {
	payload := ""
	for _, item := range os.Environ() {
		if name, value, found := strings.Cut(item, "="); found && name == fakeGitEnvironment {
			payload = value
			break
		}
	}
	if payload == "" {
		return
	}
	encoded, err := base64.StdEncoding.DecodeString(payload)
	if err != nil {
		os.Exit(2)
	}
	var fixture fakeGitFixture
	if err := json.Unmarshal(encoded, &fixture); err != nil {
		os.Exit(2)
	}
	argsPayload := ""
	for _, argument := range os.Environ() {
		if name, value, found := strings.Cut(argument, "="); found && name == fakeGitArgsEnvironment {
			argsPayload = value
			break
		}
	}
	if argsPayload == "" {
		os.Exit(2)
	}
	encodedArgs, err := base64.StdEncoding.DecodeString(argsPayload)
	if err != nil {
		os.Exit(2)
	}
	var gitArgs []string
	if err := json.Unmarshal(encodedArgs, &gitArgs); err != nil {
		os.Exit(2)
	}
	commandName, directory := "", ""
	for index, argument := range gitArgs {
		if argument == "-C" && index+1 < len(gitArgs) {
			directory = gitArgs[index+1]
		}
		switch argument {
		case "rev-parse", "status", "archive", "ls-tree":
			commandName = argument
		}
	}
	if commandName == "" {
		os.Exit(2)
	}
	response := fixture.Responses[commandName]
	if commandName == "ls-tree" && fixture.RootTreePath != "" && filepath.Clean(directory) == filepath.Clean(fixture.RootTreePath) {
		response.Stdout = fixture.RootTree
	}
	_, _ = os.Stdout.Write(response.Stdout)
	_, _ = os.Stderr.Write(response.Stderr)
	os.Exit(response.Exit)
}
