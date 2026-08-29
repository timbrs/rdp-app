package main

import (
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows/registry"
)

const (
	progID       = "rdpkey.rdp"
	friendlyName = "RDP RemoteApp Hotkeys"
	progIDTitle  = "Удалённый рабочий стол (rdpkey)"
)

func installedExePath() string {
	return filepath.Join(appDataDir(), "rdpkey.exe")
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

func regSetString(path, name, value string) error {
	k, _, err := registry.CreateKey(registry.CURRENT_USER, path, registry.SET_VALUE)
	if err != nil {
		return err
	}
	defer k.Close()
	return k.SetStringValue(name, value)
}

// sameSize: обе цели существуют и одного размера (дешёвый признак «копия
// актуальна» — при смене версии размер exe меняется).
func sameSize(a, b string) bool {
	sa, err := os.Stat(a)
	if err != nil {
		return false
	}
	sb, err := os.Stat(b)
	if err != nil {
		return false
	}
	return sa.Size() == sb.Size()
}

// Копирует exe в LocalAppData и регистрирует rdpkey для .rdp (только HKCU:
// прогид + список «Открыть с помощью»). Проверяет запись; при неуспехе — ошибка.
// Идемпотентно: если уже зарегистрированы и установленная копия совпадает с
// текущим exe по размеру — сразу выходим (вызывается на каждом запуске GUI).
func installAssociation() (string, error) {
	cur, err := os.Executable()
	if err != nil {
		return "", err
	}
	target := installedExePath()
	if associationRegistered() && sameSize(cur, target) {
		return target, nil
	}
	if !strings.EqualFold(cur, target) {
		if err := copyFile(cur, target); err != nil {
			// Целевой exe может быть занят (запущен через ассоциацию). Если копия
			// уже есть — используем её; если нет — это реальная ошибка.
			if _, e := os.Stat(target); e != nil {
				return "", err
			}
		}
	}

	cmd := "\"" + target + "\" \"%1\""
	writes := []struct{ path, name, value string }{
		{`Software\Classes\` + progID, "", progIDTitle},
		{`Software\Classes\` + progID + `\DefaultIcon`, "", target + ",0"},
		{`Software\Classes\` + progID + `\shell\open\command`, "", cmd},
		{`Software\Classes\.rdp\OpenWithProgids`, progID, ""},
		{`Software\Classes\Applications\rdpkey.exe\shell\open\command`, "", cmd},
		{`Software\Classes\Applications\rdpkey.exe`, "FriendlyAppName", friendlyName},
	}
	for _, w := range writes {
		if err := regSetString(w.path, w.name, w.value); err != nil {
			return "", err
		}
	}
	shChangeNotifyAssoc()
	if !associationRegistered() {
		return "", errors.New("запись в реестр не подтвердилась")
	}
	return target, nil
}

// Проверяет, что rdpkey зарегистрирован для .rdp (значение в OpenWithProgids).
func associationRegistered() bool {
	k, err := registry.OpenKey(registry.CURRENT_USER, `Software\Classes\.rdp\OpenWithProgids`, registry.QUERY_VALUE)
	if err != nil {
		return false
	}
	defer k.Close()
	_, _, err = k.GetStringValue(progID)
	return err == nil
}


