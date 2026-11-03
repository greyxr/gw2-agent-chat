package bridge

import (
	"bufio"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

type fakeAgent struct {
	mu       sync.Mutex
	prompts  []string
	aborts   int
	response string
	entered  chan struct{}
	release  chan struct{}
	err      error
}

func (f *fakeAgent) EnsureSession(context.Context) error { return nil }

func (f *fakeAgent) Prompt(ctx context.Context, text string) (string, error) {
	f.mu.Lock()
	f.prompts = append(f.prompts, text)
	f.mu.Unlock()
	if f.entered != nil {
		select {
		case f.entered <- struct{}{}:
		default:
		}
	}
	if f.release != nil {
		select {
		case <-ctx.Done():
			return "", ctx.Err()
		case <-f.release:
		}
	}
	return f.response, f.err
}

func (f *fakeAgent) Abort(context.Context) error {
	f.mu.Lock()
	f.aborts++
	f.mu.Unlock()
	return nil
}

func TestMessageAuthOriginLimitsUTF8AndSSE(t *testing.T) {
	agent := &fakeAgent{response: "first\nsecond\r\nthird"}
	service, token, err := NewServer(agent)
	if err != nil || len(token) != 43 {
		t.Fatalf("NewServer token: len=%d err=%v", len(token), err)
	}
	h := service.Handler()

	call := func(auth, origin, contentType string, body []byte) *httptest.ResponseRecorder {
		req := httptest.NewRequest(http.MethodPost, "/message", strings.NewReader(string(body)))
		req.Header.Set("Content-Type", contentType)
		if auth != "" {
			req.Header.Set("Authorization", auth)
		}
		if origin != "" {
			req.Header.Set("Origin", origin)
		}
		w := httptest.NewRecorder()
		h.ServeHTTP(w, req)
		return w
	}
	if got := call("", "", "text/plain; charset=utf-8", []byte("hi")).Code; got != http.StatusUnauthorized {
		t.Fatalf("missing token status = %d", got)
	}
	if got := call("Bearer "+token, "https://evil.example", "text/plain; charset=utf-8", []byte("hi")).Code; got != http.StatusForbidden {
		t.Fatalf("Origin status = %d", got)
	}
	if got := call("Bearer "+token, "", "application/json", []byte("hi")).Code; got != http.StatusUnsupportedMediaType {
		t.Fatalf("content type status = %d", got)
	}
	if got := call("Bearer "+token, "", "text/plain; charset=utf-8", bytesOf(0xff)).Code; got != http.StatusBadRequest {
		t.Fatalf("invalid UTF-8 status = %d", got)
	}
	if got := call("Bearer "+token, "", "text/plain; charset=utf-8", make([]byte, maxPromptBytes+1)).Code; got != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversize status = %d", got)
	}
	w := call("Bearer "+token, "", "text/plain; charset=utf-8", []byte("hello"))
	if w.Code != http.StatusOK || !strings.Contains(w.Header().Get("Content-Type"), "text/event-stream") {
		t.Fatalf("message response: code=%d headers=%v", w.Code, w.Header())
	}
	for _, expected := range []string{"event: status\ndata: working\n\n", "event: message\ndata: first\ndata: second\ndata: third\n\n", "event: done\ndata: \n\n"} {
		if !strings.Contains(w.Body.String(), expected) {
			t.Fatalf("SSE missing %q in %q", expected, w.Body.String())
		}
	}
}

func bytesOf(b byte) []byte { return []byte{b} }

func TestBusyCancellationAndAbort(t *testing.T) {
	agent := &fakeAgent{entered: make(chan struct{}, 1), release: make(chan struct{})}
	service, token, _ := NewServer(agent)
	server := httptest.NewServer(service.Handler())
	defer server.Close()
	ctx, cancel := context.WithCancel(context.Background())
	req, _ := http.NewRequestWithContext(ctx, http.MethodPost, server.URL+"/message", strings.NewReader("first"))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "text/plain; charset=utf-8")
	started := make(chan struct{})
	go func() {
		resp, err := server.Client().Do(req)
		if err == nil {
			_, _ = io.Copy(io.Discard, resp.Body)
			resp.Body.Close()
		}
		close(started)
	}()
	select {
	case <-agent.entered:
	case <-time.After(time.Second):
		t.Fatal("first request did not reach agent")
	}
	busyReq, _ := http.NewRequest(http.MethodPost, server.URL+"/message", strings.NewReader("second"))
	busyReq.Header.Set("Authorization", "Bearer "+token)
	busyReq.Header.Set("Content-Type", "text/plain; charset=utf-8")
	busy, err := server.Client().Do(busyReq)
	if err != nil {
		t.Fatal(err)
	}
	busy.Body.Close()
	if busy.StatusCode != http.StatusConflict {
		t.Fatalf("busy status = %d", busy.StatusCode)
	}
	cancel()
	select {
	case <-started:
	case <-time.After(2 * time.Second):
		t.Fatal("canceled request did not finish")
	}
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		agent.mu.Lock()
		aborts := agent.aborts
		agent.mu.Unlock()
		if aborts > 0 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("agent session was not aborted")
}

func TestHeartbeatWhilePromptRuns(t *testing.T) {
	agent := &fakeAgent{release: make(chan struct{}), response: "done"}
	service, token, _ := NewServer(agent)
	server := httptest.NewServer(service.Handler())
	defer server.Close()
	req, _ := http.NewRequest(http.MethodPost, server.URL+"/message", strings.NewReader("slow"))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "text/plain; charset=utf-8")
	resp, err := server.Client().Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	reader := bufio.NewReader(resp.Body)
	found := false
	deadline := time.After(2 * time.Second)
	for !found {
		line := make(chan string, 1)
		go func() {
			value, _ := reader.ReadString('\n')
			line <- value
		}()
		select {
		case value := <-line:
			if strings.HasPrefix(value, ": heartbeat") {
				found = true
			}
		case <-deadline:
			close(agent.release)
			t.Fatal("heartbeat was not sent")
		}
	}
	close(agent.release)
	_, _ = io.Copy(io.Discard, resp.Body)
}

func TestAgentFailureDoesNotLeakDetails(t *testing.T) {
	agent := &fakeAgent{err: context.DeadlineExceeded}
	service, token, _ := NewServer(agent)
	req := httptest.NewRequest(http.MethodPost, "/message", strings.NewReader("private prompt"))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "text/plain; charset=utf-8")
	w := httptest.NewRecorder()
	service.Handler().ServeHTTP(w, req)
	if strings.Contains(w.Body.String(), "DeadlineExceeded") || !strings.Contains(w.Body.String(), "agent request failed") {
		t.Fatalf("backend details leaked or safe error missing: %q", w.Body.String())
	}
}

func TestConnectionFilePermissionsAndSymlinkRefusal(t *testing.T) {
	dir := t.TempDir()
	if err := os.Chmod(dir, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "connection.conf")
	if err := WriteConnectionFile(path, "127.0.0.1:47153", "token", false); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(path)
	if err != nil || (runtime.GOOS != "windows" && info.Mode().Perm() != 0600) {
		t.Fatalf("connection file mode=%v err=%v", info.Mode(), err)
	}
	target := filepath.Join(dir, "target")
	if err := os.WriteFile(target, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(dir, "link")
	if err := os.Symlink(target, link); err != nil {
		t.Fatal(err)
	}
	if err := WriteConnectionFile(link, "127.0.0.1:47153", "token", false); err == nil {
		t.Fatal("symlink target was accepted")
	}
	data, _ := os.ReadFile(target)
	if string(data) != "keep" {
		t.Fatalf("symlink target changed: %q", data)
	}
}

func TestAddressValidation(t *testing.T) {
	for _, value := range []string{"127.0.0.1:47153", "[::1]:47153"} {
		if err := ValidateListenAddress(value); err != nil {
			t.Errorf("valid listen address %q: %v", value, err)
		}
	}
	for _, value := range []string{"localhost:47153", "0.0.0.0:47153", "127.0.0.2:47153", "127.10.20.30:47153", "[::ffff:127.0.0.1]:47153", "192.0.2.1:47153"} {
		if err := ValidateListenAddress(value); err == nil {
			t.Errorf("accepted non-loopback listen address %q", value)
		}
	}
	for _, value := range []string{"http://127.0.0.1:4096", "http://[::1]:4096"} {
		if _, err := ValidateAgentURL(value); err != nil {
			t.Errorf("valid agent URL %q: %v", value, err)
		}
	}
	for _, value := range []string{"http://localhost:4096", "http://192.0.2.1:4096", "http://127.0.0.1:4096/path", "http://127.0.0.1:4096?", "http://127.0.0.1:4096#", "http://user@127.0.0.1:4096"} {
		if _, err := ValidateAgentURL(value); err == nil {
			t.Errorf("accepted unsafe agent URL %q", value)
		}
	}
}
