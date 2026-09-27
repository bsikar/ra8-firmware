// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	"strings"
	"testing"
)

// defines reports what the gate makes of one source body for one veneer name.
func defines(name, source string) bool {
	return definesVeneer(name, []byte(source))
}

func TestABodyAfterTheParameterListIsADefinition(t *testing.T) {
	if !defines("ra8_nsc_ok", "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void) { return 0; }\n") {
		t.Fatal("a real definition was not accepted")
	}
}

func TestABraceOnTheNextLineIsADefinition(t *testing.T) {
	if !defines("ra8_nsc_ok", "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(uint8_t ch)\n{\n  return 0;\n}\n") {
		t.Fatal("the house brace style was not accepted")
	}
}

func TestAForwardDeclarationInTheSourceIsNotADefinition(t *testing.T) {
	if defines("ra8_nsc_ok", "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void);\n") {
		t.Fatal("a prototype in the source passed as a definition")
	}
}

func TestAPrototypeAboveTheDefinitionStillFindsTheDefinition(t *testing.T) {
	source := "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void);\n\nRA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void)\n{\n  return 0;\n}\n"
	if !defines("ra8_nsc_ok", source) {
		t.Fatal("the scan stopped at the prototype instead of reading on")
	}
}

func TestTheAnnotatedNameInsideACommentIsNotADefinition(t *testing.T) {
	source := "/* RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void) is implemented in the port. */\n"
	if defines("ra8_nsc_ok", source) {
		t.Fatal("a commented-out declarator passed as a definition")
	}
}

func TestACommentBetweenTheListAndTheBodyIsSkipped(t *testing.T) {
	source := "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void) /* NS entry */ { return 0; }\n"
	if !defines("ra8_nsc_ok", source) {
		t.Fatal("a comment before the body was read as something else")
	}
}

func TestALineCommentBetweenTheListAndTheBodyIsSkipped(t *testing.T) {
	source := "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void) // NS entry\n{\n  return 0;\n}\n"
	if !defines("ra8_nsc_ok", source) {
		t.Fatal("a line comment before the body was read as something else")
	}
}

func TestANestedParameterListIsFollowedToItsOwnClose(t *testing.T) {
	source := "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void (*done)(uint8_t code), uint8_t ch)\n{\n  return 0;\n}\n"
	if !defines("ra8_nsc_ok", source) {
		t.Fatal("a function-pointer parameter broke the parenthesis matching")
	}
}

func TestAnUnterminatedParameterListIsNotADefinition(t *testing.T) {
	if defines("ra8_nsc_ok", "RA8_NSC_VENEER ra8_err_t ra8_nsc_ok(void\n") {
		t.Fatal("a truncated declarator passed as a definition")
	}
}

func TestAnotherVeneersBodyDoesNotCountForThisName(t *testing.T) {
	source := "RA8_NSC_VENEER ra8_err_t ra8_nsc_other(void)\n{\n  return 0;\n}\n"
	if defines("ra8_nsc_ok", source) {
		t.Fatal("one veneer's body was read as another's")
	}
}

func TestMatchingParenReportsTheFileEnding(t *testing.T) {
	if got := matchingParen("foo(bar", 3); got != -1 {
		t.Fatalf("matchingParen() = %d, want -1", got)
	}
}

func TestAPrototypeOnlySourceIsReportedAsAPhantom(t *testing.T) {
	root := tree(t, map[string]string{
		"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n",
	}, map[string]string{
		"nsc.c": "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n",
	})
	code, out := scan(t, root)
	if code != 1 {
		t.Fatalf("exit=%d, want 1; output=%s", code, out)
	}
	if !strings.Contains(out, "ra8_nsc_phantom") {
		t.Fatalf("output did not name the phantom veneer: %s", out)
	}
}
