// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import "testing"

// Whether a file reads as a boot translation unit decides whether rule B, the
// wide "any discarded ra8_* result" rule, is switched on over it at all. Two
// ways that decision can go wrong are pinned here: a name welded to a longer
// identifier read as the entry point, and an entry point sitting at the very
// first byte of the file not read as one.

// The word boundary cannot see this on its own: an underscore is a word
// character, so \b never fires between "my_" and "SystemInit". A helper called
// my_SystemInit() is somebody else's function, and reading it as the boot
// entry point would widen rule B over a file that boots nothing.
func TestANameWeldedToALongerIdentifierIsNotTheEntryPoint(t *testing.T) {
	body := " {\n\t(void)ra8_cgc_init();\n}\n"
	for name, source := range map[string]string{
		"an underscore prefix": "void my_SystemInit(void)" + body,
		"a letter prefix":      "void xSystemInit(void)" + body,
		"a digit prefix":       "void r8SystemInit(void)" + body,
		"a welded trustzone":   "void board_ra8_trustzone_init(void)" + body,
		"an uppercase prefix":  "void XSystemInit(void)" + body,
	} {
		if definesBootEntry(source) {
			t.Fatalf("%s was read as a boot entry point", name)
		}
	}
}

// The same text with nothing welded in front of it is the entry point, so the
// rule above is a boundary check rather than a blanket refusal.
func TestTheSameDefinitionUnweldedIsTheEntryPoint(t *testing.T) {
	body := " {\n\t(void)ra8_cgc_init();\n}\n"
	for name, source := range map[string]string{
		"with a return type":  "void SystemInit(void)" + body,
		"after a separator":   "static void SystemInit(void)" + body,
		"the trustzone entry": "void ra8_trustzone_init(void)" + body,
	} {
		if !definesBootEntry(source) {
			t.Fatalf("%s was not read as a boot entry point", name)
		}
	}
}

// A definition can start at byte zero, with no preceding character to judge.
// The welding check has to answer that on its own rather than reading behind
// the start of the file.
func TestAnEntryPointAtTheFirstByteIsTheEntryPoint(t *testing.T) {
	if precededByIdentifierRune("SystemInit(void) {}", 0) {
		t.Fatal("the first byte of a file was read as welded to something")
	}
	if !definesBootEntry("SystemInit(void)\n{\n\t(void)ra8_cgc_init();\n}\n") {
		t.Fatal("an entry point at the first byte was not read as one")
	}
	if !precededByIdentifierRune("my_SystemInit(void) {}", 3) {
		t.Fatal("a welded name was not reported as welded")
	}
	for name, before := range map[string]string{
		"a space":       "void SystemInit(void) {}",
		"an asterisk":   "void *SystemInit(void) {}",
		"a parenthesis": "f((SystemInit(void)) {}",
	} {
		index := len(before) - len("SystemInit(void) {}")
		if name == "a parenthesis" {
			index = 3
		}
		if index < 0 {
			t.Fatalf("%s: the fixture is shorter than the name", name)
		}
		if precededByIdentifierRune(before, index) {
			t.Fatalf("%s before the name was read as welding", name)
		}
	}
}

// A file that only declares the entry point emits nothing, so rule B must stay
// off over it even though the name is present twice.
func TestADeclarationAloneDoesNotSwitchTheWideRuleOn(t *testing.T) {
	if definesBootEntry("void SystemInit(void);\nvoid ra8_trustzone_init(void);\n") {
		t.Fatal("a header of declarations was read as a boot translation unit")
	}
}
