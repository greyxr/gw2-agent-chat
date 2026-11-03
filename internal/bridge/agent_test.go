package bridge

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestOpenCodeSessionReuseAndEscapedPath(t *testing.T) {
	creates := 0
	messages := 0
	wantPrompts := []string{"first", "second"}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		user, pass, ok := r.BasicAuth()
		if !ok || user != "bridge-user" || pass != "backend-secret" {
			t.Errorf("unexpected backend credentials")
		}
		switch {
		case r.Method == http.MethodPost && r.URL.Path == "/session":
			creates++
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(`{"id":"ses/opaque"}`))
		case r.Method == http.MethodPost && r.URL.EscapedPath() == "/session/ses%2Fopaque/message":
			messages++
			var body struct {
				Agent string `json:"agent"`
				Parts []struct {
					Type string `json:"type"`
					Text string `json:"text"`
				} `json:"parts"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Agent != "gw2" || len(body.Parts) != 1 || body.Parts[0].Type != "text" || body.Parts[0].Text != wantPrompts[messages-1] {
				t.Errorf("unexpected prompt payload: %#v err=%v", body, err)
			}
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(`{"info":{},"parts":[{"type":"text","text":"answer"},{"type":"text","text":" two"}]}`))
		default:
			t.Errorf("unexpected request %s %s (%s)", r.Method, r.URL.Path, r.URL.EscapedPath())
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	agent, err := NewOpenCodeAgent(server.URL, "bridge-user", "backend-secret")
	if err != nil {
		t.Fatal(err)
	}
	for _, prompt := range []string{"first", "second"} {
		answer, err := agent.Prompt(context.Background(), prompt)
		if err != nil || answer != "answer two" {
			t.Fatalf("Prompt(%q) = %q, %v", prompt, answer, err)
		}
	}
	if creates != 1 || messages != 2 {
		t.Fatalf("creates=%d messages=%d", creates, messages)
	}
}

func TestOpenCodeAgentDoesNotFollowRedirect(t *testing.T) {
	redirected := false
	second := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { redirected = true }))
	defer second.Close()
	first := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, second.URL, http.StatusFound)
	}))
	defer first.Close()
	agent, err := NewOpenCodeAgent(first.URL, "u", "p")
	if err != nil {
		t.Fatal(err)
	}
	err = agent.EnsureSession(context.Background())
	if err == nil || redirected || strings.Contains(err.Error(), "p") {
		t.Fatalf("redirect was followed or details leaked: err=%v redirected=%v", err, redirected)
	}
}
