package server

import (
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func boundTask() catalog.Task {
	return catalog.Task{Name: "hil-run", ArgsSchema: catalog.ArgsSchema{
		Positional: []string{"target"}, Flags: []string{"profile"}}}
}

// TestASubmittedArgvMustBeTheBindingOfItsValues is the gap this file closes:
// a request states argv and values separately, and only the values were ever
// read into the row. Stating one thing and persisting another is refused.
func TestASubmittedArgvMustBeTheBindingOfItsValues(t *testing.T) {
	definition := boundTask()
	values := map[string]string{"target": "ra8p1", "profile": "release"}
	agreeing := taskRequest{Name: "hil-run", Args: []string{"ra8p1", "--profile=release"}, Values: values}
	if err := checkSubmittedArgumentsAreBound(definition, agreeing); err != nil {
		t.Fatalf("a request whose argv is the binding was refused: %v", err)
	}
	disagreeing := taskRequest{Name: "hil-run", Args: []string{"ra8d2", "--profile=release"}, Values: values}
	if err := checkSubmittedArgumentsAreBound(definition, disagreeing); err == nil {
		t.Fatal("a request stating an argv its values do not bind was admitted")
	}
}

// TestTheOldPositionalRuleStillHoldsForAnArgumentFreeTask pins that every task
// in the v1 catalog is judged exactly as before: no schema binds nothing, so
// an argv element is still refused and an empty one still admitted.
func TestTheOldPositionalRuleStillHoldsForAnArgumentFreeTask(t *testing.T) {
	definition := catalog.Task{Name: "test-go"}
	for _, requested := range []taskRequest{
		{Name: "test-go"},
		{Name: "test-go", Args: []string{}},
	} {
		if err := checkSubmittedArgumentsAreBound(definition, requested); err != nil {
			t.Fatalf("an argument-free submission was refused: %v", err)
		}
	}
	stated := taskRequest{Name: "test-go", Args: []string{"-race"}}
	if err := checkSubmittedArgumentsAreBound(definition, stated); err == nil {
		t.Fatal("a submitter stated an argv element for a task that declares none")
	}
}

// TestAnAbsentArgvIsTheSameStatementAsAnEmptyOne: the shape every request
// written before the values key existed has.
func TestAnAbsentArgvIsTheSameStatementAsAnEmptyOne(t *testing.T) {
	definition := boundTask()
	values := map[string]string{"target": "ra8p1"}
	absent := taskRequest{Name: "hil-run", Values: values}
	if err := checkSubmittedArgumentsAreBound(definition, absent); err == nil {
		t.Fatal("an absent argv was admitted beside values that bind one element")
	}
	empty := taskRequest{Name: "hil-run", Args: []string{}, Values: values}
	if errors.Is(checkSubmittedArgumentsAreBound(definition, absent), store.ErrInvalid) !=
		errors.Is(checkSubmittedArgumentsAreBound(definition, empty), store.ErrInvalid) {
		t.Fatal("an absent argv and an empty argv were judged differently")
	}
}

// TestValuesTheCatalogRefusesAreRefusedHere: the binding is the rule, so its
// own refusals travel rather than being re-stated.
func TestValuesTheCatalogRefusesAreRefusedHere(t *testing.T) {
	definition := boundTask()
	for name, values := range map[string]map[string]string{
		"undeclared": {"target": "ra8p1", "quiet": "1"},
		"missing":    {"profile": "release"},
		"shell":      {"target": "a;rm -rf /"},
	} {
		requested := taskRequest{Name: "hil-run", Values: values}
		if err := checkSubmittedArgumentsAreBound(definition, requested); err == nil {
			t.Fatalf("%s values were admitted at submission", name)
		}
	}
}

// TestARefusalNamesTheTaskAndIsInvalid: a submitter sends several tasks in one
// run, and the refusal is all the caller and the log get.
func TestARefusalNamesTheTaskAndIsInvalid(t *testing.T) {
	definition := boundTask()
	requested := taskRequest{Name: "hil-run", Args: []string{"ra8p1"}, Values: map[string]string{"target": "ra8d2"}}
	err := checkSubmittedArgumentsAreBound(definition, requested)
	if err == nil {
		t.Fatal("a disagreeing submission was admitted")
	}
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("refusal = %v, want store.ErrInvalid", err)
	}
	if want := "hil-run"; !contains(err.Error(), want) {
		t.Fatalf("refusal %q does not name the task", err)
	}
}

// TestAnArgvLongerThanTheBindingIsRefused: the length is judged in both
// directions, so extra trailing elements cannot ride along.
func TestAnArgvLongerThanTheBindingIsRefused(t *testing.T) {
	definition := boundTask()
	requested := taskRequest{
		Name:   "hil-run",
		Args:   []string{"ra8p1", "--profile=release"},
		Values: map[string]string{"target": "ra8p1"},
	}
	if err := checkSubmittedArgumentsAreBound(definition, requested); err == nil {
		t.Fatal("an argv longer than the binding was admitted")
	}
}

// TestWhatThePlanePersistsIsWhatWasAdmitted closes the loop: a submission this
// file admits writes a row that re-validates against the same catalog.
func TestWhatThePlanePersistsIsWhatWasAdmitted(t *testing.T) {
	definition := boundTask()
	values := map[string]string{"target": "ra8p1", "profile": "release"}
	requested := taskRequest{Name: "hil-run", Args: []string{"ra8p1", "--profile=release"}, Values: values}
	if err := checkSubmittedArgumentsAreBound(definition, requested); err != nil {
		t.Fatal(err)
	}
	if _, err := persistedTaskArguments(definition, requested.Values); err != nil {
		t.Fatalf("an admitted submission did not persist: %v", err)
	}
	if err := definition.ValidatePersistedArguments(values, requested.Args); err != nil {
		t.Fatalf("the admitted argv does not re-validate as a persisted row: %v", err)
	}
}

func contains(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}
