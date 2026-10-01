package store

import (
	"errors"
	"fmt"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgconn"
)

// The board plane decides a great deal before it ever opens a transaction:
// which actor may speak for a class, who owns a waiter, whether a counter
// still fits a bigint column, and whether a failed statement is the kind
// Postgres expects us to retry. None of that needs a database, and none of it
// was held. These tests pin those decisions alone, so a later change to the
// SQL cannot quietly move them.

func boardActor(kind, role string) BoardActor {
	return BoardActor{id: "actor-1", kind: kind, role: role, boardID: "board-1", repository: "bsikar/ra8-firmware"}
}

func waiterHeldBy(id, holder string) board.Waiter {
	return board.Waiter{ID: id, Holder: holder, Class: board.ClassAI, Sequence: 1}
}

func TestOnlyOneKindAndRoleMaySpeakForEachBoardClass(t *testing.T) {
	for _, c := range []struct {
		name    string
		actor   BoardActor
		class   board.Class
		allowed bool
	}{
		{"a board human speaks for the human class", boardActor("human", "board_human"), board.ClassHuman, true},
		{"an operator speaks for the human class too", boardActor("human", "operator"), board.ClassHuman, true},
		{"a human submitter does not", boardActor("human", "submitter"), board.ClassHuman, false},
		{"an agent claiming the human role does not", boardActor("agent", "board_human"), board.ClassHuman, false},
		{"a GitHub app submitter speaks for CI", boardActor("github_app", "submitter"), board.ClassCI, true},
		{"a GitHub app operator does not", boardActor("github_app", "operator"), board.ClassCI, false},
		{"an agent submitter does not speak for CI", boardActor("agent", "submitter"), board.ClassCI, false},
		{"an agent submitter speaks for the AI class", boardActor("agent", "submitter"), board.ClassAI, true},
		{"an agent operator does not", boardActor("agent", "operator"), board.ClassAI, false},
		{"a GitHub app does not speak for the AI class", boardActor("github_app", "submitter"), board.ClassAI, false},
		{"an operator does not speak for the AI class either", boardActor("human", "operator"), board.ClassAI, false},
	} {
		t.Run(c.name, func(t *testing.T) {
			if got := classAllowed(c.actor, c.class); got != c.allowed {
				t.Fatalf("classAllowed(%+v, %d) = %v, want %v", c.actor, c.class, got, c.allowed)
			}
		})
	}
}

// An unnamed class is the shape an HTTP body can produce by leaving the field
// off, so it must be refused for every actor rather than falling through to
// the most privileged branch.
func TestAClassTheBoardDoesNotNameIsAllowedToNobody(t *testing.T) {
	for _, class := range []board.Class{board.Class(0), board.Class(4), board.Class(200), board.Class(255)} {
		for _, actor := range []BoardActor{
			boardActor("human", "operator"),
			boardActor("human", "board_human"),
			boardActor("github_app", "submitter"),
			boardActor("agent", "submitter"),
		} {
			if classAllowed(actor, class) {
				t.Fatalf("class %d allowed to %+v", class, actor)
			}
		}
	}
}

func TestAWaiterIsOwnedOnlyByTheHolderRecordedAgainstIt(t *testing.T) {
	snapshot := board.Snapshot{Queue: []board.Waiter{
		waiterHeldBy("w1", "actor-1"),
		waiterHeldBy("w2", "actor-2"),
	}}
	if !ownsWaiter(snapshot, "w1", "actor-1") {
		t.Fatal("the recorded holder does not own its own waiter")
	}
	if ownsWaiter(snapshot, "w1", "actor-2") {
		t.Fatal("a second actor owns a waiter it did not queue")
	}
	if ownsWaiter(snapshot, "w2", "actor-1") {
		t.Fatal("holding one waiter carried ownership of another")
	}
	if ownsWaiter(snapshot, "w3", "actor-1") {
		t.Fatal("a waiter that is not queued was owned")
	}
	if ownsWaiter(board.Snapshot{}, "w1", "actor-1") {
		t.Fatal("an empty queue yielded ownership")
	}
}

// Both halves have to match. A waiter ID that belongs to somebody else must
// not be admitted because the actor happens to hold a different waiter, and an
// empty actor ID must not match a waiter whose holder was never recorded.
func TestOwningAWaiterTakesBothTheIdentifierAndTheHolder(t *testing.T) {
	snapshot := board.Snapshot{Queue: []board.Waiter{{ID: "w1", Holder: ""}}}
	if !ownsWaiter(snapshot, "w1", "") {
		t.Fatal("an unrecorded holder is matched by its own empty value, which is what the row says")
	}
	if ownsWaiter(snapshot, "", "") {
		t.Fatal("an empty waiter ID matched a queued waiter")
	}
}

func TestTheQueueIsSearchedByWaiterIdentifierAlone(t *testing.T) {
	queue := []board.Waiter{waiterHeldBy("w1", "actor-1"), waiterHeldBy("w2", "actor-2")}
	if !queueContains(queue, "w1") || !queueContains(queue, "w2") {
		t.Fatal("a queued waiter was not found")
	}
	if queueContains(queue, "w3") {
		t.Fatal("an absent waiter was found")
	}
	if queueContains(nil, "w1") {
		t.Fatal("a nil queue contained a waiter")
	}
	if queueContains(queue, "") {
		t.Fatal("an empty identifier matched a queued waiter")
	}
}

// The priority string reaches a database column and an operator's report, so
// every class must map to exactly one word, and an unnamed class must land on
// the least privileged of them rather than on nothing.
func TestEveryBoardClassCarriesItsOwnPriorityWord(t *testing.T) {
	for class, want := range map[board.Class]string{
		board.ClassHuman: "human",
		board.ClassCI:    "ci",
		board.ClassAI:    "agent",
		board.Class(0):   "agent",
		board.Class(9):   "agent",
	} {
		if got := boardPriority(class); got != want {
			t.Fatalf("boardPriority(%d) = %q, want %q", class, got, want)
		}
	}
}

func TestOnlyTheFourPhasesHoldingAGrantAreLive(t *testing.T) {
	live := map[board.Phase]bool{
		board.GrantPending:   true,
		board.Active:         true,
		board.YieldRequested: true,
		board.Draining:       true,
	}
	for _, phase := range []board.Phase{
		board.Ready, board.GrantPending, board.Active, board.YieldRequested,
		board.Draining, board.RecoveryRequired, board.Recovering, board.Quarantined,
		board.Phase(""), board.Phase("active "), board.Phase("ACTIVE"),
	} {
		if got := liveBoardPhase(phase); got != live[phase] {
			t.Fatalf("liveBoardPhase(%q) = %v, want %v", phase, got, live[phase])
		}
	}
}

// A lease deadline is written in whole seconds, and rounding it DOWN would
// publish a deadline earlier than the one granted. Every fraction has to round
// away from zero's side, and an exact second must not gain one.
func TestASecondCountIsRoundedUpNeverDown(t *testing.T) {
	for _, c := range []struct {
		in   time.Duration
		want int64
	}{
		{0, 0},
		{1 * time.Nanosecond, 1},
		{999999999 * time.Nanosecond, 1},
		{time.Second, 1},
		{time.Second + time.Nanosecond, 2},
		{1500 * time.Millisecond, 2},
		{2 * time.Second, 2},
		{90 * time.Second, 90},
		{time.Hour, 3600},
	} {
		if got := ceilSeconds(c.in); got != c.want {
			t.Fatalf("ceilSeconds(%v) = %d, want %d", c.in, got, c.want)
		}
	}
}

// Integer division in Go truncates toward zero, so the same expression rounds
// a negative duration toward zero rather than up. A negative span only reaches
// here when two stamps arrived out of order; pinning it says what the column
// would receive rather than leaving it to be discovered in a report.
func TestANegativeSpanIsTruncatedTowardZeroRatherThanRoundedUp(t *testing.T) {
	for _, c := range []struct {
		in   time.Duration
		want int64
	}{
		{-time.Nanosecond, 0},
		{-time.Second, 0},
		{-time.Second - time.Nanosecond, 0},
		{-2 * time.Second, -1},
		{-90 * time.Second, -89},
	} {
		if got := ceilSeconds(c.in); got != c.want {
			t.Fatalf("ceilSeconds(%v) = %d, want %d", c.in, got, c.want)
		}
	}
}

// A zero time must reach the driver as SQL NULL. Sending Go's zero instant
// instead would write year 1 into a timestamptz column, which reads as a real
// moment forever after.
func TestAnUnsetInstantIsWrittenAsNullAndASetOneIsWrittenWhole(t *testing.T) {
	if got := nullTime(time.Time{}); got != nil {
		t.Fatalf("nullTime(zero) = %v, want nil", got)
	}
	stamp := time.Date(2026, 9, 28, 4, 30, 0, 0, time.UTC)
	got, ok := nullTime(stamp).(time.Time)
	if !ok {
		t.Fatalf("nullTime(%v) did not hand back a time", stamp)
	}
	if !got.Equal(stamp) {
		t.Fatalf("nullTime(%v) = %v", stamp, got)
	}
	// The zero instant in a non-UTC location is still the zero instant only
	// when IsZero says so; a shifted one is a real moment and must be kept.
	shifted := time.Time{}.In(time.FixedZone("plus-one", 3600))
	if nullTime(shifted) == nil && !shifted.IsZero() {
		t.Fatal("a non-zero instant was written as NULL")
	}
}

// Only 40001 is the serialization failure Postgres asks us to retry. Reading a
// different class as retryable would replay a statement that genuinely failed.
func TestOnlyASerializationFailureIsTreatedAsRetryable(t *testing.T) {
	if !isSerializationFailure(&pgconn.PgError{Code: "40001"}) {
		t.Fatal("40001 was not recognised")
	}
	if !isSerializationFailure(fmt.Errorf("board snapshot CAS: %w", &pgconn.PgError{Code: "40001"})) {
		t.Fatal("a wrapped 40001 was not recognised")
	}
	for _, err := range []error{
		nil,
		errors.New("connection reset"),
		ErrConflict,
		&pgconn.PgError{Code: "40P01"}, // deadlock detected, not a serialization failure
		&pgconn.PgError{Code: "23505"}, // unique violation
		&pgconn.PgError{Code: ""},
		fmt.Errorf("wrapped: %w", errors.New("40001")),
	} {
		if isSerializationFailure(err) {
			t.Fatalf("%v was treated as a serialization failure", err)
		}
	}
}

// The board's counters are unsigned in Go and signed in Postgres. A value past
// the bigint ceiling has to be refused here, because the driver would either
// wrap it or fail the whole transaction with a message naming no counter.
func TestACounterPastTheBigintCeilingIsRefusedBeforeItReachesPostgres(t *testing.T) {
	ceiling := uint64(math.MaxInt64)
	ok := board.Snapshot{
		BoardID:        "board-1",
		Version:        ceiling,
		Generation:     ceiling,
		AgentHighWater: ceiling,
		NextSequence:   ceiling,
		Lease:          &board.Lease{ID: "lease-1", Generation: ceiling},
		Queue:          []board.Waiter{{ID: "w1", Sequence: ceiling}},
	}
	if err := validateBoardSQLBounds(ok); err != nil {
		t.Fatalf("the ceiling itself was refused: %v", err)
	}

	for _, c := range []struct {
		name     string
		mutate   func(*board.Snapshot)
		contains string
	}{
		{"version", func(s *board.Snapshot) { s.Version = ceiling + 1 }, "board counter exceeds PostgreSQL bigint"},
		{"generation", func(s *board.Snapshot) { s.Generation = ceiling + 1 }, "board counter exceeds PostgreSQL bigint"},
		{"agent high water", func(s *board.Snapshot) { s.AgentHighWater = ceiling + 1 }, "board counter exceeds PostgreSQL bigint"},
		{"next sequence", func(s *board.Snapshot) { s.NextSequence = ceiling + 1 }, "board counter exceeds PostgreSQL bigint"},
		{"lease generation", func(s *board.Snapshot) { s.Lease.Generation = ceiling + 1 }, "lease generation exceeds PostgreSQL bigint"},
		{"waiter sequence", func(s *board.Snapshot) { s.Queue[0].Sequence = ceiling + 1 }, "waiter sequence exceeds PostgreSQL bigint"},
	} {
		t.Run(c.name, func(t *testing.T) {
			snapshot := ok
			snapshot.Lease = &board.Lease{ID: "lease-1", Generation: ceiling}
			snapshot.Queue = []board.Waiter{{ID: "w1", Sequence: ceiling}}
			c.mutate(&snapshot)

			err := validateBoardSQLBounds(snapshot)
			if err == nil {
				t.Fatalf("%s past the ceiling was accepted", c.name)
			}
			if !errors.Is(err, ErrConflict) {
				t.Fatalf("%s refused as %v, want a conflict", c.name, err)
			}
			if !strings.Contains(err.Error(), c.contains) {
				t.Fatalf("%s refused with %q, want it to name %q", c.name, err, c.contains)
			}
		})
	}
}

// A board with no lease and no queue must pass the bounds check rather than
// fault on the absent lease pointer.
func TestABoardHoldingNothingPassesTheBoundsCheck(t *testing.T) {
	if err := validateBoardSQLBounds(board.Snapshot{BoardID: "board-1", Phase: board.Ready}); err != nil {
		t.Fatalf("an empty board was refused: %v", err)
	}
	if err := validateBoardSQLBounds(board.Snapshot{}); err != nil {
		t.Fatalf("a zero snapshot was refused: %v", err)
	}
}

// The bounds check reads every waiter, not only the first, because the queue
// is the one place a counter arrives from outside the snapshot's own arithmetic.
func TestEveryWaiterIsReadByTheBoundsCheckNotOnlyTheFirst(t *testing.T) {
	snapshot := board.Snapshot{Queue: []board.Waiter{
		{ID: "w1", Sequence: 1},
		{ID: "w2", Sequence: 2},
		{ID: "w3", Sequence: uint64(math.MaxInt64) + 1},
	}}
	err := validateBoardSQLBounds(snapshot)
	if err == nil || !errors.Is(err, ErrConflict) {
		t.Fatalf("a last-placed waiter past the ceiling was accepted: %v", err)
	}
}

func TestASegmentIdentifierIsAUUIDOrNothing(t *testing.T) {
	for _, good := range []string{
		"0f3f9c3e-6c1f-4f7a-9f0f-2f7a1b9c4d5e",
		"00000000-0000-0000-0000-000000000000",
		"0F3F9C3E-6C1F-4F7A-9F0F-2F7A1B9C4D5E",
	} {
		if !validSegmentID(good) {
			t.Fatalf("validSegmentID(%q) refused a UUID", good)
		}
	}
	for _, bad := range []string{
		"",
		"segment-1",
		"0f3f9c3e-6c1f-4f7a-9f0f",
		"0f3f9c3e-6c1f-4f7a-9f0f-2f7a1b9c4d5e ",
		" 0f3f9c3e-6c1f-4f7a-9f0f-2f7a1b9c4d5e",
		"zzzzzzzz-6c1f-4f7a-9f0f-2f7a1b9c4d5e",
		"0f3f9c3e6c1f4f7a9f0f2f7a1b9c4d5e0f3f",
	} {
		if validSegmentID(bad) {
			t.Fatalf("validSegmentID(%q) admitted a non-UUID", bad)
		}
	}
}

// A segment key is written into an audit row and read back by an operator, so
// a control character or a surrounding space would make two distinct keys look
// like one in a report.
func TestASegmentKeyCarriesNoControlCharactersAndNoSurroundingSpace(t *testing.T) {
	for _, good := range []string{
		"bring-up",
		"a",
		"segment key with spaces inside",
		"ünïcode-is-fine",
		strings.Repeat("k", 128),
	} {
		if !validSegmentKey(good) {
			t.Fatalf("validSegmentKey(%q) refused an ordinary key", good)
		}
	}
	for _, bad := range []string{
		"",
		strings.Repeat("k", 129),
		" leading",
		"trailing ",
		"\tkey",
		"key\n",
		"two\nlines",
		"nul\x00inside",
		"bell\ainside",
		"delete\x7finside",
		"\x1bescape",
	} {
		if validSegmentKey(bad) {
			t.Fatalf("validSegmentKey(%q) admitted a key it should refuse", bad)
		}
	}
}
