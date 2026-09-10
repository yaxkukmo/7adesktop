// Package syncclient trzyma wywolania HTTP do 7asyncd - odpowiednik
// internal/httpapi, ale po stronie klienta.
package syncclient

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"

	"7adesktop/sync/internal/store"
)

type Client struct {
	baseURL string
	apiKey  string
	http    *http.Client
}

// New tworzy klienta HTTP(S) do 7asyncd. caCertPath to opcjonalna sciezka
// do PEM self-signed certu serwera (zasob "ca_cert" w sync.conf) - potrzebne
// tylko wtedy, gdy serwer stoi za relayd/nginx z certem, ktoremu system nie
// ufa domyslnie (brak publicznej domeny -> brak Let's Encrypt, patrz
// TODO.md sekcja TLS). Pusty caCertPath = domyslne zaufanie systemowe
// (zwykly http:// albo https:// z prawdziwym certem CA).
func New(baseURL, apiKey, caCertPath string) (*Client, error) {
	httpClient := &http.Client{}
	if caCertPath != "" {
		pem, err := os.ReadFile(caCertPath)
		if err != nil {
			return nil, fmt.Errorf("reading ca_cert %s: %w", caCertPath, err)
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(pem) {
			return nil, fmt.Errorf("ca_cert %s: invalid PEM", caCertPath)
		}
		httpClient.Transport = &http.Transport{
			TLSClientConfig: &tls.Config{RootCAs: pool},
		}
	}

	return &Client{
		baseURL: strings.TrimRight(baseURL, "/"),
		apiKey:  apiKey,
		http:    httpClient,
	}, nil
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
		return nil, fmt.Errorf("server returned %s: %s", resp.Status, readErrBody(resp.Body))
	}

	var items []store.Item
	if err := json.NewDecoder(resp.Body).Decode(&items); err != nil {
		return nil, fmt.Errorf("decoding response: %w", err)
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
		return fmt.Errorf("server returned %s: %s", resp.Status, readErrBody(resp.Body))
	}
	return nil
}

func readErrBody(r io.Reader) string {
	b, _ := io.ReadAll(io.LimitReader(r, 1<<10))
	return strings.TrimSpace(string(b))
}
