package bridge

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

const maxAgentResponse = 2 << 20

type Agent interface {
	EnsureSession(context.Context) error
	Prompt(context.Context, string) (string, error)
	Abort(context.Context) error
}

type OpenCodeAgent struct {
	base     string
	username string
	password string
	client   *http.Client
	mu       sync.Mutex
	session  string
}

func NewOpenCodeAgent(baseURL, username, password string) (*OpenCodeAgent, error) {
	u, err := ValidateAgentURL(baseURL)
	if err != nil {
		return nil, err
	}
	transport := &http.Transport{
		Proxy:                 nil,
		DialContext:           (&net.Dialer{Timeout: 3 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
		TLSHandshakeTimeout:   3 * time.Second,
		ResponseHeaderTimeout: 180 * time.Second,
		MaxIdleConns:          2,
		IdleConnTimeout:       30 * time.Second,
	}
	return &OpenCodeAgent{
		base: normalizeAgentBase(u), username: username, password: password,
		client: &http.Client{Transport: transport, Timeout: 180 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }},
	}, nil
}

func (a *OpenCodeAgent) EnsureSession(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.session != "" {
		return nil
	}
	var data struct {
		ID string `json:"id"`
	}
	if err := a.requestJSON(ctx, http.MethodPost, "/session", map[string]string{"agent": "gw2"}, &data); err != nil {
		return err
	}
	if !validSessionID(data.ID) {
		return errors.New("invalid agent session response")
	}
	a.session = data.ID
	return nil
}

func (a *OpenCodeAgent) Prompt(ctx context.Context, prompt string) (string, error) {
	if err := a.EnsureSession(ctx); err != nil {
		return "", err
	}
	a.mu.Lock()
	session := a.session
	a.mu.Unlock()
	body := map[string]any{
		"agent": "gw2",
		"parts": []map[string]string{{"type": "text", "text": prompt}},
	}
	path := "/session/" + url.PathEscape(session) + "/message"
	var result struct {
		Parts []struct {
			Type string `json:"type"`
			Text string `json:"text"`
		} `json:"parts"`
	}
	if err := a.requestJSON(ctx, http.MethodPost, path, body, &result); err != nil {
		return "", err
	}
	var out strings.Builder
	for _, part := range result.Parts {
		if part.Type == "text" {
			out.WriteString(part.Text)
		}
	}
	if out.Len() == 0 {
		return "", errors.New("agent returned no text")
	}
	return out.String(), nil
}

func (a *OpenCodeAgent) Abort(ctx context.Context) error {
	a.mu.Lock()
	session := a.session
	a.mu.Unlock()
	if session == "" {
		return nil
	}
	path := "/session/" + url.PathEscape(session) + "/abort"
	return a.requestJSON(ctx, http.MethodPost, path, nil, nil)
}

func (a *OpenCodeAgent) requestJSON(ctx context.Context, method, path string, input, output any) error {
	var body io.Reader
	if input != nil {
		encoded, err := json.Marshal(input)
		if err != nil {
			return errors.New("could not encode agent request")
		}
		body = bytes.NewReader(encoded)
	}
	req, err := http.NewRequestWithContext(ctx, method, a.base+path, body)
	if err != nil {
		return errors.New("could not create agent request")
	}
	req.SetBasicAuth(a.username, a.password)
	if input != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := a.client.Do(req)
	if err != nil {
		return errors.New("agent request failed")
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("agent returned HTTP %d", resp.StatusCode)
	}
	if output == nil {
		return nil
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, maxAgentResponse+1))
	if err != nil || len(data) > maxAgentResponse {
		return errors.New("agent response could not be read")
	}
	if err := json.Unmarshal(data, output); err != nil {
		return errors.New("agent returned an invalid response")
	}
	return nil
}

func validSessionID(id string) bool {
	if !strings.HasPrefix(id, "ses") || len(id) < 4 || len(id) > 128 {
		return false
	}
	for _, r := range id {
		if r < 0x20 || r == 0x7f {
			return false
		}
	}
	return true
}
