package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"gw2agentchat/internal/bridge"
)

func main() {
	listen := flag.String("listen", "127.0.0.1:47153", "loopback listen address")
	agentURL := flag.String("agent-url", "http://127.0.0.1:4096", "loopback OpenCode server URL")
	connection := flag.String("connection-file", "", "private connection file path")
	flag.Parse()
	if err := bridge.ValidateListenAddress(*listen); err != nil {
		log.Fatal(err)
	}
	username := os.Getenv("OPENCODE_SERVER_USERNAME")
	if username == "" {
		username = "opencode"
	}
	password := os.Getenv("OPENCODE_SERVER_PASSWORD")
	if password == "" {
		log.Fatal("OPENCODE_SERVER_PASSWORD is required")
	}
	agent, err := bridge.NewOpenCodeAgent(*agentURL, username, password)
	if err != nil {
		log.Fatal(err)
	}
	service, token, err := bridge.NewServer(agent)
	if err != nil {
		log.Fatal("could not generate bridge pairing token")
	}
	connectionPath := *connection
	createDirectory := false
	if connectionPath == "" {
		connectionPath, err = bridge.DefaultConnectionPath()
		if err != nil {
			log.Fatal(err)
		}
		createDirectory = true
	}
	listener, err := net.Listen("tcp", *listen)
	if err != nil {
		log.Fatal("could not listen on loopback address")
	}
	address := listener.Addr().String()
	if err := bridge.WriteConnectionFile(connectionPath, address, token, createDirectory); err != nil {
		listener.Close()
		log.Fatal(err)
	}

	root, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	server := &http.Server{
		Handler:           service.Handler(),
		ReadTimeout:       5 * time.Second,
		ReadHeaderTimeout: 5 * time.Second,
		IdleTimeout:       30 * time.Second,
		MaxHeaderBytes:    8 << 10,
		BaseContext:       func(net.Listener) context.Context { return root },
	}
	done := make(chan error, 1)
	go func() { done <- server.Serve(listener) }()
	select {
	case err := <-done:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatal("bridge server stopped")
		}
	case <-root.Done():
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := server.Shutdown(ctx); err != nil {
			_ = server.Close()
		}
		if err := <-done; err != nil && !errors.Is(err, http.ErrServerClosed) {
			fmt.Fprintln(os.Stderr, "bridge server stopped")
		}
	}
}
