// Package syncclient trzyma wywolania HTTP do 7asyncd - odpowiednik
// internal/httpapi, ale po stronie klienta.
package syncclient

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"

	"7adesktop/sync/internal/store"
)

type Client struct {
	baseURL string
	apiKey  string
	http    *http.Client
}

func New(baseURL, apiKey string) *Client {
	return &Client{
		baseURL: strings.TrimRight(baseURL, "/"),
		apiKey:  apiKey,
		http:    &http.Client{},
	}
}

// Pull pobiera GET /api/items?since=<since>.
func (c *Client) Pull(ctx context.Context, since int64) ([]store.Item, error) {
	url := fmt.Sprintf("%s/api/items?since=%d", c.baseURL, since)

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("X-API-Key", c.apiKey)

	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("serwer zwrocil %s: %s", resp.Status, readErrBody(resp.Body))
	}

	var items []store.Item
	if err := json.NewDecoder(resp.Body).Decode(&items); err != nil {
		return nil, fmt.Errorf("dekodowanie odpowiedzi: %w", err)
	}
	return items, nil
}

// PushBatch wysyla POST /api/items/batch.
func (c *Client) PushBatch(ctx context.Context, items []store.Item) error {
	body, err := json.Marshal(items)
	if err != nil {
		return err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/api/items/batch", bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("X-API-Key", c.apiKey)
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("serwer zwrocil %s: %s", resp.Status, readErrBody(resp.Body))
	}
	return nil
}

func readErrBody(r io.Reader) string {
	b, _ := io.ReadAll(io.LimitReader(r, 1<<10))
	return strings.TrimSpace(string(b))
}
