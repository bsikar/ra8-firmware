package store

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestSlowTasksRejectsUnscopedOrUnboundedRequest(t *testing.T) {
	if _, err := (*Store)(nil).SlowTasks(context.Background(), "repo", time.Now(), 10); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("nil store error=%v", err)
	}
	s := &Store{pool: nil}
	if _, err := s.SlowTasks(context.Background(), "repo", time.Now(), 10); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("missing pool error=%v", err)
	}
}
