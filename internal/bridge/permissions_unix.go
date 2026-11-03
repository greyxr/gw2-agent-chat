//go:build !windows

package bridge

import "os"

func privateDirectory(info os.FileInfo) bool {
	return info.Mode().Perm()&0077 == 0
}

func protectConnectionFile(_ string, file *os.File) error {
	return file.Chmod(0600)
}
