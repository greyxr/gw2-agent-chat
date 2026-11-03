//go:build windows

package bridge

import (
	"os"
	"path/filepath"
	"testing"
	"unsafe"

	"golang.org/x/sys/windows"
)

const fileAllAccess = 0x001F01FF

func TestConnectionFileACL(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "connection.conf")
	if err := WriteConnectionFile(path, "127.0.0.1:47153", "token", false); err != nil {
		t.Fatal(err)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		t.Fatal(err)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		t.Fatal(err)
	}
	control, _, err := sd.Control()
	if err != nil || control&windows.SE_DACL_PROTECTED == 0 || dacl.AceCount != 1 {
		t.Fatalf("ACL is not protected or has unexpected entries: control=%x entries=%d err=%v", control, dacl.AceCount, err)
	}
	ace := new(windows.ACCESS_ALLOWED_ACE)
	if err := windows.GetAce(dacl, 0, &ace); err != nil {
		t.Fatal(err)
	}
	if ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE || uint32(ace.Mask)&fileAllAccess != fileAllAccess {
		t.Fatalf("unexpected token-file ACE: type=%d mask=%x", ace.Header.AceType, ace.Mask)
	}
	gotSID := (*windows.SID)(unsafe.Pointer(&ace.SidStart))
	token, err := windows.OpenCurrentProcessToken()
	if err != nil {
		t.Fatal(err)
	}
	defer token.Close()
	user, err := token.GetTokenUser()
	if err != nil || !gotSID.Equals(user.User.Sid) {
		t.Fatalf("connection ACL does not grant the current user: err=%v", err)
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatal(err)
	}
}
