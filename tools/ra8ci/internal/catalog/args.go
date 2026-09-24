// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"sort"
	"strings"
)

// The argument contract, stated once.
//
// A task's arguments are NAMED and reviewed: the catalog declares which names
// exist, a caller supplies values by name, and binding turns the pair into
// argv elements. There is no point in this path at which a command exists as
// a string, so there is nothing for a shell to re-split, expand or chain.
// That is the whole security argument, and it is a property of the shape of
// this API rather than of the values that travel through it.
//
// The value rules below are the second line, not the first. They are a
// conservative allowlist because every argument a reviewed task takes today
// is a path, an identifier or a switch, and a value outside that set is far
// more likely to be a caller's mistake than a requirement. Relaxing them for
// a task that genuinely needs a wider alphabet is a catalog review, and the
// argv shape means such a task is not thereby made unsafe.
const (
	// MaxPositionalArguments and MaxFlagArguments bound one task's schema.
	MaxPositionalArguments = 8
	MaxFlagArguments       = 16
	// MaxArgumentNameBytes bounds a declared name.
	MaxArgumentNameBytes = 48
	// MaxArgumentValueBytes bounds one supplied value. A path is the longest
	// value a reviewed task takes, so this is sized off a path, not a payload.
	MaxArgumentValueBytes = 512
)

// ValidArgumentName is the rule for a name the catalog declares. It is
// validName plus a leading letter and a bound, so a declared name is always
// usable as a long flag: --name, never --0 or --trailing-.
func ValidArgumentName(name string) bool {
	if len(name) == 0 || len(name) > MaxArgumentNameBytes {
		return false
	}
	if name[0] < 'a' || name[0] > 'z' {
		return false
	}
	if strings.HasSuffix(name, "-") || strings.Contains(name, "--") {
		return false
	}
	return validName(name)
}

// ValidArgumentValue is the rule for a value a caller supplies.
//
// Printable ASCII only, so no NUL, newline, carriage return, tab or escape
// can ride into an argv element, a log line or an audit row. No shell
// metacharacter, per the allowlist argument above. No leading dash, because a
// value that looks like an option is the one value a program is most likely
// to read as something other than a value. No surrounding whitespace, since a
// value that differs from its own trimmed form is ambiguous evidence about
// what was requested.
func ValidArgumentValue(value string) bool {
	if value == "" || len(value) > MaxArgumentValueBytes {
		return false
	}
	if strings.TrimSpace(value) != value || strings.HasPrefix(value, "-") {
		return false
	}
	for index := 0; index < len(value); index++ {
		char := value[index]
		if char < 0x20 || char > 0x7e {
			return false
		}
		if strings.IndexByte(shellMetacharacters, char) >= 0 {
			return false
		}
	}
	return true
}

// shellMetacharacters is every byte that means something to a POSIX shell or
// to cmd.exe. None of them can reach an argv element this package builds.
const shellMetacharacters = "|&;<>()$`\\\"'\t\n\r*?[]{}!~^%"

// ValidateArgsSchema checks the shape of a declared schema: every name is
// usable, no name is declared twice, and one name is never both a positional
// and a flag. The last rule matters because binding would otherwise have to
// choose which of the two a supplied value meant.
func ValidateArgsSchema(schema ArgsSchema) error {
	if len(schema.Positional) > MaxPositionalArguments {
		return fmt.Errorf("%w: at most %d positional arguments", ErrInvalidCatalog, MaxPositionalArguments)
	}
	if len(schema.Flags) > MaxFlagArguments {
		return fmt.Errorf("%w: at most %d flag arguments", ErrInvalidCatalog, MaxFlagArguments)
	}
	declared := make(map[string]bool, len(schema.Positional)+len(schema.Flags))
	for _, group := range [][]string{schema.Positional, schema.Flags} {
		for _, name := range group {
			if !ValidArgumentName(name) {
				return fmt.Errorf("%w: invalid argument name %q", ErrInvalidCatalog, name)
			}
			if declared[name] {
				return fmt.Errorf("%w: argument %q is declared twice", ErrInvalidCatalog, name)
			}
			declared[name] = true
		}
	}
	return nil
}

// ArgumentNames lists every name a schema declares, positionals first in
// declared order and then flags, so an error message or a CLI usage line can
// name what a task actually accepts.
func (schema ArgsSchema) ArgumentNames() []string {
	names := make([]string, 0, len(schema.Positional)+len(schema.Flags))
	names = append(names, schema.Positional...)
	return append(names, schema.Flags...)
}

// BindArguments turns named values into argv for a task with this schema.
//
// Positionals come first, in the order the catalog declared them, as bare
// values; every one is required, because a positional that may be absent
// would silently shift the ones after it. Flags follow as --name=value in
// declared order, and a flag a caller did not supply is simply absent. One
// element per argument, always: the = form means a value can never be read as
// the next flag even if the value rules are one day relaxed.
//
// An unknown name is refused rather than dropped. A caller naming an argument
// the reviewed task does not declare has misunderstood the task, and running
// it anyway would run something other than what was asked for.
func BindArguments(schema ArgsSchema, values map[string]string) ([]string, error) {
	if err := ValidateArgsSchema(schema); err != nil {
		return nil, err
	}
	declared := make(map[string]bool, len(schema.Positional)+len(schema.Flags))
	for _, name := range schema.ArgumentNames() {
		declared[name] = true
	}
	unknown := make([]string, 0, len(values))
	for name := range values {
		if !declared[name] {
			unknown = append(unknown, name)
		}
	}
	if len(unknown) != 0 {
		sort.Strings(unknown)
		return nil, fmt.Errorf("%w: undeclared argument(s) %s", ErrInvalidCatalog, strings.Join(unknown, ", "))
	}
	argv := make([]string, 0, len(schema.Positional)+len(schema.Flags))
	for _, name := range schema.Positional {
		value, supplied := values[name]
		if !supplied {
			return nil, fmt.Errorf("%w: positional argument %q is required", ErrInvalidCatalog, name)
		}
		if !ValidArgumentValue(value) {
			return nil, fmt.Errorf("%w: invalid value for %q", ErrInvalidCatalog, name)
		}
		argv = append(argv, value)
	}
	for _, name := range schema.Flags {
		value, supplied := values[name]
		if !supplied {
			continue
		}
		if !ValidArgumentValue(value) {
			return nil, fmt.Errorf("%w: invalid value for %q", ErrInvalidCatalog, name)
		}
		argv = append(argv, "--"+name+"="+value)
	}
	return argv, nil
}

// BindArguments binds a caller's named values against this task's reviewed
// schema. A task declaring no arguments accepts none, which is every task in
// the v1 catalog: ValidateTask still refuses a definition that declares a
// schema, so this is the contract a reviewed task will bind against rather
// than a path any embedded task reaches today.
func (t Task) BindArguments(values map[string]string) ([]string, error) {
	if len(t.ArgsSchema.Positional) == 0 && len(t.ArgsSchema.Flags) == 0 {
		if len(values) != 0 {
			return nil, fmt.Errorf("%w: task %q accepts no arguments", ErrInvalidCatalog, t.Name)
		}
		return nil, nil
	}
	argv, err := BindArguments(t.ArgsSchema, values)
	if err != nil {
		return nil, fmt.Errorf("%w (task %q)", err, t.Name)
	}
	return argv, nil
}

// StepArgv is the exact argv a step runs once a task's arguments are bound:
// the reviewed arguments from the catalog, then the bound ones. It copies
// rather than appending in place, so a caller cannot reach back through the
// result and mutate the reviewed definition it came from.
//
// Reviewed arguments come first and bound ones last because the reviewed part
// is what a reader of the catalog can see, and a bound value must not be able
// to land in front of it and change what the earlier arguments mean.
func StepArgv(step Step, bound []string) ([]string, error) {
	argv := make([]string, 0, len(step.Args)+len(bound))
	for _, arg := range step.Args {
		if strings.ContainsRune(arg, '\x00') {
			return nil, fmt.Errorf("%w: NUL in reviewed argument", ErrInvalidCatalog)
		}
		argv = append(argv, arg)
	}
	for _, arg := range bound {
		if arg == "" || strings.ContainsAny(arg, "\x00\r\n") {
			return nil, fmt.Errorf("%w: invalid bound argument", ErrInvalidCatalog)
		}
		argv = append(argv, arg)
	}
	return argv, nil
}
