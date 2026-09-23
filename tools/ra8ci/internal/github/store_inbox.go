package github

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// InboxStore is the narrow PostgreSQL persistence contract used by the
// scale-set controller. It allows unit tests without a database.
type InboxStore interface {
	SaveGitHubMessage(context.Context, string, string, string, json.RawMessage) error
	MarkGitHubMessageProcessed(context.Context, string, string, string) error
	ListPendingGitHubMessagesForScaleSet(context.Context, string, int) ([]store.GitHubMessage, error)
}

// StoreInbox adapts the transactional store to the listener's replay contract.
type StoreInbox struct {
	backend    InboxStore
	scaleSetID int
}

func NewStoreInbox(backend InboxStore, scaleSetID int) (*StoreInbox, error) {
	if backend == nil || scaleSetID <= 0 {
		return nil, errors.New("github inbox requires store and positive scale-set ID")
	}
	return &StoreInbox{backend: backend, scaleSetID: scaleSetID}, nil
}

func (i *StoreInbox) Save(ctx context.Context, message Message) error {
	if i == nil || message.ScaleSetID != i.scaleSetID || message.SessionID == "" || message.MessageID <= 0 {
		return errors.New("github inbox identity mismatch")
	}
	payload, err := json.Marshal(message)
	if err != nil {
		return err
	}
	return i.backend.SaveGitHubMessage(ctx, strconv.Itoa(message.ScaleSetID), message.SessionID, strconv.Itoa(message.MessageID), payload)
}

func (i *StoreInbox) Pending(ctx context.Context, limit int) ([]Message, error) {
	if i == nil || limit < 1 || limit > 1000 {
		return nil, errors.New("invalid github inbox page")
	}
	rows, err := i.backend.ListPendingGitHubMessagesForScaleSet(ctx, strconv.Itoa(i.scaleSetID), limit)
	if err != nil {
		return nil, err
	}
	result := make([]Message, 0, len(rows))
	for _, row := range rows {
		var message Message
		if err := json.Unmarshal(row.Payload, &message); err != nil {
			return nil, fmt.Errorf("decode github message: %w", err)
		}
		if row.ScaleSetID != strconv.Itoa(i.scaleSetID) || row.SessionID != message.SessionID || row.MessageID != strconv.Itoa(message.MessageID) || message.ScaleSetID != i.scaleSetID {
			return nil, errors.New("github inbox payload identity mismatch")
		}
		result = append(result, message)
	}
	return result, nil
}

func (i *StoreInbox) MarkProcessed(ctx context.Context, message Message) error {
	if i == nil || message.ScaleSetID != i.scaleSetID || message.SessionID == "" || message.MessageID <= 0 {
		return errors.New("github inbox identity mismatch")
	}
	return i.backend.MarkGitHubMessageProcessed(ctx, strconv.Itoa(i.scaleSetID), message.SessionID, strconv.Itoa(message.MessageID))
}
