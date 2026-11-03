package bridge

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"io"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"
)

const maxPromptBytes = 16 << 10

type Server struct {
	Token  string
	Agent  Agent
	active chan struct{}
}

func NewServer(agent Agent) (*Server, string, error) {
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return nil, "", err
	}
	token := base64.RawURLEncoding.EncodeToString(raw[:])
	return &Server{Token: token, Agent: agent, active: make(chan struct{}, 1)}, token, nil
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", s.health)
	mux.HandleFunc("POST /message", s.message)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) { http.NotFound(w, r) })
	return mux
}

func (s *Server) health(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/health" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, "ok\n")
}

func (s *Server) message(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Origin") != "" {
		http.Error(w, "browser requests are not accepted", http.StatusForbidden)
		return
	}
	if !authorized(r.Header.Get("Authorization"), s.Token) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	mediaType, params, typeErr := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if typeErr != nil || mediaType != "text/plain" || !strings.EqualFold(params["charset"], "utf-8") {
		http.Error(w, "content type must be text/plain; charset=utf-8", http.StatusUnsupportedMediaType)
		return
	}
	limited := http.MaxBytesReader(w, r.Body, maxPromptBytes)
	input, err := io.ReadAll(limited)
	if err != nil {
		var maxErr *http.MaxBytesError
		if errors.As(err, &maxErr) {
			http.Error(w, "message is too large", http.StatusRequestEntityTooLarge)
		} else {
			http.Error(w, "could not read message", http.StatusBadRequest)
		}
		return
	}
	if !utf8.Valid(input) {
		http.Error(w, "message must be valid UTF-8", http.StatusBadRequest)
		return
	}
	if len(input) == 0 || strings.TrimSpace(string(input)) == "" {
		http.Error(w, "message is empty", http.StatusBadRequest)
		return
	}
	select {
	case s.active <- struct{}{}:
	default:
		http.Error(w, "another message is active", http.StatusConflict)
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		<-s.active
		http.Error(w, "streaming is unavailable", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache, no-transform")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(http.StatusOK)
	writeSSE(w, "status", "working")
	flusher.Flush()

	type result struct {
		text string
		err  error
	}
	finished := make(chan result, 1)
	go func() {
		defer func() { <-s.active }()
		if err := s.Agent.EnsureSession(r.Context()); err != nil {
			if r.Context().Err() != nil {
				s.abort()
			}
			finished <- result{err: err}
			return
		}
		text, err := s.Agent.Prompt(r.Context(), string(input))
		if r.Context().Err() != nil {
			s.abort()
		}
		finished <- result{text: text, err: err}
	}()

	ticker := time.NewTicker(500 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case result := <-finished:
			if result.err != nil {
				writeSSE(w, "error", "agent request failed")
			} else {
				writeSSE(w, "message", result.text)
			}
			writeSSE(w, "done", "")
			flusher.Flush()
			return
		case <-ticker.C:
			_, _ = io.WriteString(w, ": heartbeat\n\n")
			flusher.Flush()
		case <-r.Context().Done():
			cleanup := time.NewTimer(4 * time.Second)
			defer cleanup.Stop()
			select {
			case <-finished:
			case <-cleanup.C:
			}
			return
		}
	}
}

func (s *Server) abort() {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = s.Agent.Abort(ctx)
}

func authorized(header, token string) bool {
	const prefix = "Bearer "
	if !strings.HasPrefix(header, prefix) {
		return false
	}
	provided := []byte(strings.TrimPrefix(header, prefix))
	wanted := []byte(token)
	return len(provided) == len(wanted) && subtle.ConstantTimeCompare(provided, wanted) == 1
}

func writeSSE(w io.Writer, event, data string) {
	_, _ = io.WriteString(w, "event: "+event+"\n")
	data = strings.ReplaceAll(strings.ReplaceAll(data, "\r\n", "\n"), "\r", "\n")
	for _, line := range strings.Split(data, "\n") {
		_, _ = io.WriteString(w, "data: "+line+"\n")
	}
	_, _ = io.WriteString(w, "\n")
}

func DefaultConnectionPath() (string, error) {
	root, err := os.UserConfigDir()
	if err != nil || root == "" {
		return "", errors.New("user configuration directory is unavailable")
	}
	return filepath.Join(root, "gw2-agent-chat", "connection.conf"), nil
}

func WriteConnectionFile(path, address, token string, createDirectory bool) error {
	if !filepath.IsAbs(path) || token == "" || ValidateListenAddress(address) != nil {
		return errors.New("invalid connection file settings")
	}
	parent := filepath.Dir(path)
	if createDirectory {
		if err := os.Mkdir(parent, 0700); err != nil && !errors.Is(err, os.ErrExist) {
			return errors.New("could not create private connection directory")
		}
	}
	info, err := os.Lstat(parent)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || !privateDirectory(info) {
		return errors.New("connection directory must be a private directory")
	}
	if info, err := os.Lstat(path); err == nil {
		if !info.Mode().IsRegular() {
			return errors.New("connection file must be a regular file")
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return errors.New("could not inspect connection file")
	}
	tmp, err := os.CreateTemp(parent, ".connection-*")
	if err != nil {
		return errors.New("could not create connection file")
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := protectConnectionFile(tmpName, tmp); err != nil {
		tmp.Close()
		return errors.New("could not protect connection file")
	}
	content := "url=http://" + address + "\ntoken=" + token + "\n"
	if _, err := io.WriteString(tmp, content); err != nil {
		tmp.Close()
		return errors.New("could not write connection file")
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return errors.New("could not save connection file")
	}
	if err := tmp.Close(); err != nil {
		return errors.New("could not close connection file")
	}
	if err := os.Rename(tmpName, path); err != nil {
		return errors.New("could not install connection file")
	}
	return nil
}
