package main

import (
	_ "embed"
	"os"
	"path/filepath"
)

//go:embed help.html
var helpHTML string

// «Как пользоваться?»: распаковываем встроенный HTML во временный файл и
// открываем в браузере по умолчанию.
func openHelp() {
	path := filepath.Join(os.TempDir(), "rdpkey-help.html")
	if err := os.WriteFile(path, []byte(helpHTML), 0o644); err != nil {
		return
	}
	openURL(path)
}
