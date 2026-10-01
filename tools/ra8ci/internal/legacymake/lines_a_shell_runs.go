// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package legacymake

import (
	"path"
	"path/filepath"
	"strings"
)

// shebangLookback bounds the read of a file's first line. A shebang is the
// first thing in the file or it is not one, so nothing past this can change
// the answer and a large file is never copied to reach it.
const shebangLookback = 256

// commandSuffixes name the files whose lines a shell, a CI runner, or a
// container build executes. A Just recipe body belongs here: just hands each
// recipe line to a shell, so ".just" is as much a command file as ".sh", and
// this gate exists to move repository tasks onto those recipes.
var commandSuffixes = []string{".sh", ".bash", ".zsh", ".ksh", ".yml", ".yaml", ".just"}

// commandShells name the interpreters whose shebang makes a suffixless file a
// command file. Deliberately the shell family only: a Python script's lines
// are not shell commands, so the command-shaped patterns would misread them.
var commandShells = []string{"sh", "bash", "zsh", "ksh", "dash"}

// runsCommands reports whether rel's lines are commands rather than prose.
//
// A file with a suffix is judged by that suffix alone, so a documented example
// in a .md file stays prose and a .py file with a shell shebang is not
// promoted. A file with no suffix is judged by its first line, which is the
// only place its interpreter is written down: the repository's git hooks carry
// no extension and are as executable as anything under scripts/.
func runsCommands(rel string, data []byte) bool {
	base := filepath.Base(rel)
	if base == "Dockerfile" || base == "justfile" || base == ".justfile" {
		return true
	}
	suffix := strings.ToLower(filepath.Ext(rel))
	if suffix != "" {
		for _, candidate := range commandSuffixes {
			if suffix == candidate {
				return true
			}
		}
		return false
	}
	return hasShellShebang(data)
}

// hasShellShebang reports whether data begins with a shebang naming a shell.
func hasShellShebang(data []byte) bool {
	head := data
	if len(head) > shebangLookback {
		head = head[:shebangLookback]
	}
	line := string(head)
	if index := strings.IndexAny(line, "\r\n"); index >= 0 {
		line = line[:index]
	}
	if !strings.HasPrefix(line, "#!") {
		return false
	}
	for _, word := range strings.Fields(strings.TrimPrefix(line, "#!")) {
		if strings.HasPrefix(word, "-") {
			continue
		}
		// Shebangs are written with forward slashes on every platform, so the
		// interpreter name is a path element, not a host path element.
		name := path.Base(word)
		if name == "env" {
			continue
		}
		for _, shell := range commandShells {
			if name == shell {
				return true
			}
		}
		return false
	}
	return false
}
