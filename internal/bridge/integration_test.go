package bridge

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

func TestBridgeToOpenCodeSessionReuse(t *testing.T) {
	var mu sync.Mutex
	creates := 0
	var prompts []string
	agentServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		username, password, ok := r.BasicAuth()
		if !ok || username != "bridge-user" || password != "backend-secret" {
			t.Errorf("OpenCode request lacked configured Basic auth")
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		if r.Method == http.MethodPost && r.URL.Path == "/session" {
			mu.Lock()
			creates++
			mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"id":"ses_integration"}`)
			return
		}
		if r.Method == http.MethodPost && r.URL.Path == "/session/ses_integration/message" {
			var request struct {
				Agent string `json:"agent"`
				Parts []struct {
					Type string `json:"type"`
					Text string `json:"text"`
				} `json:"parts"`
			}
			if err := json.NewDecoder(r.Body).Decode(&request); err != nil || request.Agent != "gw2" || len(request.Parts) != 1 || request.Parts[0].Type != "text" {
				t.Errorf("invalid OpenCode prompt: request=%#v err=%v", request, err)
				http.Error(w, "invalid prompt", http.StatusBadRequest)
				return
			}
			mu.Lock()
			prompts = append(prompts, request.Parts[0].Text)
			mu.Unlock()
			payload, _ := json.Marshal(map[string]any{
				"info":  map[string]string{},
				"parts": []map[string]string{{"type": "text", "text": "answer: " + request.Parts[0].Text}},
			})
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write(payload)
			return
		}
		t.Errorf("unexpected OpenCode request: %s %s", r.Method, r.URL.Path)
		http.NotFound(w, r)
	}))
	defer agentServer.Close()

	agent, err := NewOpenCodeAgent(agentServer.URL, "bridge-user", "backend-secret")
	if err != nil {
		t.Fatal(err)
	}
	service, token, err := NewServer(agent)
	if err != nil {
		t.Fatal(err)
	}
	bridgeServer := httptest.NewServer(service.Handler())
	defer bridgeServer.Close()

	for _, prompt := range []string{"first prompt", "second prompt"} {
		req, err := http.NewRequestWithContext(context.Background(), http.MethodPost, bridgeServer.URL+"/message", strings.NewReader(prompt))
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("Authorization", "Bearer "+token)
		req.Header.Set("Content-Type", "text/plain; charset=utf-8")
		resp, err := bridgeServer.Client().Do(req)
		if err != nil {
			t.Fatal(err)
		}
		body, readErr := io.ReadAll(resp.Body)
		resp.Body.Close()
		if readErr != nil || resp.StatusCode != http.StatusOK {
			t.Fatalf("bridge response: status=%d err=%v", resp.StatusCode, readErr)
		}
		stream := string(body)
		for _, expected := range []string{"event: status\ndata: working\n\n", fmt.Sprintf("event: message\ndata: answer: %s\n\n", prompt), "event: done\ndata: \n\n"} {
			if !strings.Contains(stream, expected) {
				t.Fatalf("response for %q missing %q: %q", prompt, expected, stream)
			}
		}
	}

	mu.Lock()
	defer mu.Unlock()
	if creates != 1 || len(prompts) != 2 || prompts[0] != "first prompt" || prompts[1] != "second prompt" {
		t.Fatalf("OpenCode session use: creates=%d prompts=%q", creates, prompts)
	}
}
