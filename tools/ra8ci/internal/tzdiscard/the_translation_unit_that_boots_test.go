// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	"os"
	"path/filepath"
	"testing"
)

// ruleBFires writes source as a .c file and reports whether the wide boot-TU
// rule was applied to it.
func ruleBFires(t *testing.T, source string) bool {
	t.Helper()
	root := t.TempDir()
	path := filepath.Join(root, "unit.c")
	if err := os.WriteFile(path, []byte(source), 0o600); err != nil {
		t.Fatal(err)
	}
	for _, item := range checkFile("unit.c", root) {
		if item.rule == "B" {
			return true
		}
	}
	return false
}

const discard = "  (void)ra8_cgc_init();\n"

func TestThePlainDefinitionStillBoots(t *testing.T) {
	for _, header := range []string{
		"void SystemInit(void) {\n",
		"void ra8_trustzone_init(void) {\n",
		"void SystemInit(void)\n{\n",
	} {
		if !definesBootEntry(header + discard + "}\n") {
			t.Fatalf("plain definition not recognised: %q", header)
		}
	}
}

func TestADecoratedDefinitionStillBoots(t *testing.T) {
	for _, header := range []string{
		"__attribute__((used)) void SystemInit(void) {\n",
		"void\nSystemInit(void)\n{\n",
		"RA8_BOOT_SECTION void ra8_trustzone_init(void) {\n",
		"void SystemInit(void) /* entry */ {\n",
		"void SystemInit(void) // entry\n{\n",
	} {
		if !definesBootEntry(header + discard + "}\n") {
			t.Fatalf("decorated definition not recognised: %q", header)
		}
	}
}

func TestADeclarationDoesNotBoot(t *testing.T) {
	for _, source := range []string{
		"void SystemInit(void);\n",
		"extern void ra8_trustzone_init(void);\n",
		"void SystemInit(void) ;\n",
		"static const handler_t table[] = { SystemInit };\n",
	} {
		if definesBootEntry(source) {
			t.Fatalf("a declaration was read as a definition: %q", source)
		}
	}
}

func TestADeclarationAboveTheDefinitionStillBoots(t *testing.T) {
	if !definesBootEntry("void SystemInit(void);\n\nvoid SystemInit(void) {\n" + discard + "}\n") {
		t.Fatal("a prototype above the definition hid it")
	}
}

func TestALongerNameIsNotTheEntryPoint(t *testing.T) {
	for _, source := range []string{
		"void ra8_trustzone_init_late(void) {\n" + discard + "}\n",
		"void board_SystemInit(void) {\n" + discard + "}\n",
	} {
		if definesBootEntry(source) {
			t.Fatalf("a longer name was read as the entry point: %q", source)
		}
	}
}

func TestAnArgumentListThatIsNotVoidDoesNotBoot(t *testing.T) {
	if definesBootEntry("void SystemInit(int mode) {\n" + discard + "}\n") {
		t.Fatal("a different signature was read as the entry point")
	}
}

func TestAnUnterminatedCommentDoesNotOpenABody(t *testing.T) {
	if bodyOpensAt("void SystemInit(void) /* never closed", 21) {
		t.Fatal("an unterminated block comment opened a body")
	}
	if bodyOpensAt("void SystemInit(void) // never ends", 21) {
		t.Fatal("a trailing line comment opened a body")
	}
	if bodyOpensAt("void SystemInit(void)", 21) {
		t.Fatal("end of file opened a body")
	}
}

func TestRuleBFollowsTheDefinition(t *testing.T) {
	if !ruleBFires(t, "void\nSystemInit(void)\n{\n"+discard+"}\n") {
		t.Fatal("rule B did not reach a definition with its return type on the line above")
	}
	if ruleBFires(t, "void SystemInit(void);\n"+discard) {
		t.Fatal("rule B was applied to a translation unit that only declares the entry point")
	}
}

func TestRuleAIsUnaffectedByBootDetection(t *testing.T) {
	if !ruleBFires(t, "void SystemInit(void) {\n"+discard+"}\n") {
		t.Fatal("the ordinary boot TU lost rule B")
	}
	root := t.TempDir()
	path := filepath.Join(root, "ordinary.c")
	if err := os.WriteFile(path, []byte("void f(void) { (void)ra8_tz_secure_boot_verify(); }\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	findings := checkFile("ordinary.c", root)
	if len(findings) != 1 || findings[0].rule != "A" {
		t.Fatalf("rule A findings = %+v", findings)
	}
}
