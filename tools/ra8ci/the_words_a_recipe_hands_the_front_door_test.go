// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The justfile is the repository's front door for a person, and ra8ci is the
// front door for the plane. Where a recipe types a ra8ci word, the two doors
// have to agree, and nothing checks them against each other. A renamed
// command does not break its recipe loudly: run() hands any word it does not
// recognise to runLocalTask as a task name, so a recipe naming a command that
// no longer exists answers "unknown task", which reads like the operator
// mistyped rather than like the recipe went stale. A word that is still a
// catalog task but no longer a gate command is worse, because it changes what
// the recipe does without changing its exit status: it spools a local run
// instead of scanning the checkout.
//
// So this reads the real recipes out of the checkout and judges every ra8ci
// word in them against the tables the binary dispatches from.

// justInvocation is one ra8ci invocation written into a recipe, kept with
// where it was found so a failure names the line to edit.
type justInvocation struct {
	file  string
	line  int
	words []string
}

// recipeWords reads the checkout's justfile and every file under just/, and
// reports each ra8ci invocation it finds.
//
// Both spellings count. A recipe either runs the installed binary ("ra8ci
// ascii") or runs it out of the module ("go run . ascii"), and the second is
// how a recipe reaches a gate without an install step. Arguments stop the
// read at the first token that is a flag or a just interpolation, because only
// the command words are being judged: "{{ quote(manifest) }}" is a value the
// recipe passes, not a word the front door looks up.
func recipeWords(t *testing.T) []justInvocation {
	t.Helper()
	root := filepath.Join("..", "..")

	files := []string{filepath.Join(root, "justfile")}
	entries, err := os.ReadDir(filepath.Join(root, "just"))
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".just") {
			files = append(files, filepath.Join(root, "just", entry.Name()))
		}
	}

	var found []justInvocation
	for _, file := range files {
		body, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		for index, line := range strings.Split(string(body), "\n") {
			text := strings.TrimSpace(line)
			if strings.HasPrefix(text, "#") {
				continue
			}
			for _, opener := range []string{"go run . ", "ra8ci "} {
				at := strings.Index(text, opener)
				if at < 0 {
					continue
				}
				words := commandWords(text[at+len(opener):])
				if len(words) == 0 {
					continue
				}
				found = append(found, justInvocation{
					file:  filepath.ToSlash(file),
					line:  index + 1,
					words: words,
				})
			}
		}
	}
	return found
}

// commandWords takes the command words off the head of a recipe line and
// stops at the first token that cannot be one.
func commandWords(tail string) []string {
	var words []string
	for _, token := range strings.Fields(tail) {
		if strings.HasPrefix(token, "-") || strings.HasPrefix(token, "{{") {
			break
		}
		if strings.ContainsAny(token, "\"'$|&;<>()") {
			break
		}
		words = append(words, token)
	}
	return words
}

func TestTheRecipesReallyDoNameRa8ciWords(t *testing.T) {
	// Every test below passes trivially if the scan finds nothing, and a
	// recipe file renamed or a spelling changed would do exactly that. This
	// is the guard on the guard: the recipes are known to invoke ra8ci
	// several times, so an empty or thin scan is the scan being broken rather
	// than the recipes being clean.
	found := recipeWords(t)
	if len(found) < 4 {
		t.Fatalf("scanned the checkout's recipes and found %d ra8ci invocations, expected at least 4; the scan is broken", len(found))
	}

	// Both spellings have to be exercised, or half the reader is untested.
	// The module spelling lives in the local CI recipes; the installed
	// spelling is used everywhere else.
	var module, installed bool
	for _, invocation := range found {
		if strings.Contains(invocation.file, "ci_local.just") {
			module = true
			continue
		}
		installed = true
	}
	if !module || !installed {
		t.Fatalf("expected ra8ci invocations both in ci_local.just and outside it, got %v", found)
	}
}

func TestEveryRa8ciWordARecipeTypesIsOneTheFrontDoorKnows(t *testing.T) {
	doors := frontDoorWords()

	loaded, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	tasks := make(map[string]bool)
	for _, name := range loaded.Names() {
		tasks[name] = true
	}

	for _, invocation := range recipeWords(t) {
		word := invocation.words[0]
		if doors[word] || tasks[word] {
			continue
		}
		t.Errorf("%s:%d runs ra8ci %q, which is neither a front-door command nor a catalog task", invocation.file, invocation.line, word)
	}
}

func TestEveryBareRa8ciWordARecipeTypesIsATaskTheCatalogDeclares(t *testing.T) {
	// A gate word carries two jobs behind one spelling. runCheckoutGate hands
	// an invocation with arguments to the gate, to scan the checkout; an
	// invocation with none goes to runLocalTask, which looks the word up in
	// the catalog. Every ra8ci word in these recipes is written bare, so
	// every one of them takes the catalog path, and a word the catalog has
	// dropped answers "unknown task" rather than naming the stale recipe.
	//
	// Being in the front-door table is not enough to save such a word, which
	// is why this judges the catalog rather than the table: "ra8ci test-go"
	// is in no table at all and is a perfectly good recipe, while a gate word
	// whose task was renamed would still be in the table and would still
	// fail.
	loaded, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}

	for _, invocation := range recipeWords(t) {
		if len(invocation.words) != 1 {
			continue
		}
		word := invocation.words[0]
		task, declared := loaded.Task(word)
		if !declared {
			t.Errorf("%s:%d runs ra8ci %q with no arguments, which the catalog does not declare, so it answers \"unknown task\"", invocation.file, invocation.line, word)
			continue
		}
		// These recipes are what a developer and CI both type, and the tree
		// builds on linux, so a task that does not run there is a recipe
		// nobody can use.
		if !task.SupportsOS("linux") {
			t.Errorf("%s:%d runs ra8ci %q, a task the catalog does not offer on linux", invocation.file, invocation.line, word)
		}
	}
}

func TestEveryRa8ciSubcommandARecipeTypesIsOneItsCommandDispatches(t *testing.T) {
	// Only the commands carrying their own tables are judged here, each
	// against the table it dispatches from, so a subcommand renamed inside
	// one of them is caught rather than read as a stray argument.
	tables := map[string]map[string]bool{
		"hil":    hilWords(),
		"board":  boardWords(),
		"github": githubWords(),
	}

	for _, invocation := range recipeWords(t) {
		table, carries := tables[invocation.words[0]]
		if !carries {
			continue
		}
		if len(invocation.words) < 2 {
			t.Errorf("%s:%d runs ra8ci %s with no subcommand", invocation.file, invocation.line, invocation.words[0])
			continue
		}
		if table[invocation.words[1]] {
			continue
		}
		t.Errorf("%s:%d runs ra8ci %s %q, which that command does not dispatch", invocation.file, invocation.line, invocation.words[0], invocation.words[1])
	}
}

// frontDoorWords reports the top-level command names as a set.
func frontDoorWords() map[string]bool {
	names := make(map[string]bool)
	for _, command := range topLevelCommands() {
		names[command.Name] = true
	}
	return names
}

// hilWords reports the hil subcommand names as a set.
func hilWords() map[string]bool {
	names := make(map[string]bool)
	for _, subcommand := range hilSubcommands() {
		names[subcommand.Name] = true
	}
	return names
}

// boardWords reports the board subcommand names as a set.
func boardWords() map[string]bool {
	names := make(map[string]bool)
	for _, subcommand := range boardSubcommands() {
		names[subcommand.Name] = true
	}
	return names
}

// githubWords reports the github subcommand names as a set.
func githubWords() map[string]bool {
	names := make(map[string]bool)
	for _, subcommand := range githubSubcommands() {
		names[subcommand.Name] = true
	}
	return names
}
