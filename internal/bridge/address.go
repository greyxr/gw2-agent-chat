package bridge

import (
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"
)

func ValidateListenAddress(value string) error {
	host, port, err := net.SplitHostPort(value)
	if err != nil {
		return fmt.Errorf("listen address must be an IP address and port")
	}
	if host != "127.0.0.1" && host != "::1" {
		return fmt.Errorf("listen address must use 127.0.0.1 or ::1")
	}
	n, err := strconv.Atoi(port)
	if err != nil || n < 0 || n > 65535 {
		return fmt.Errorf("listen port is invalid")
	}
	return nil
}

func ValidateAgentURL(value string) (*url.URL, error) {
	if strings.ContainsAny(value, "?#") {
		return nil, fmt.Errorf("agent URL must not include query or fragment data")
	}
	u, err := url.Parse(value)
	if err != nil || u == nil || u.Scheme != "http" || u.User != nil || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" || u.RawFragment != "" || (u.Path != "" && u.Path != "/") {
		return nil, fmt.Errorf("agent URL must be a plain loopback HTTP URL")
	}
	host := u.Hostname()
	ip := net.ParseIP(host)
	if ip == nil || !ip.IsLoopback() || u.Port() == "" {
		return nil, fmt.Errorf("agent URL must use a loopback IP address and port")
	}
	port, err := strconv.Atoi(u.Port())
	if err != nil || port < 1 || port > 65535 {
		return nil, fmt.Errorf("agent URL port is invalid")
	}
	u.Path = ""
	u.RawPath = ""
	return u, nil
}

func normalizeAgentBase(u *url.URL) string {
	return strings.TrimRight(u.String(), "/")
}
