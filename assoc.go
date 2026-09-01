package main

import (
	"os"

	"golang.org/x/sys/windows/registry"
)

const (
	progID       = "rdpkey.rdp"
	friendlyName = "RDP RemoteApp Hotkeys"
	progIDTitle  = "Удалённый рабочий стол (rdpkey)"
)

func regSetString(path, name, value string) error {
	k, _, err := registry.CreateKey(registry.CURRENT_USER, path, registry.SET_VALUE)
	if err != nil {
		return err
	}
	defer k.Close()
	return k.SetStringValue(name, value)
}

// installAssociation прописывает rdpkey в «Открыть с помощью» для .rdp (только
// HKCU, без прав админа, НЕ как приложение по умолчанию). Ассоциирует ТЕКУЩИЙ
// exe — где бы он ни лежал; себя НЕ копирует. Идемпотентно: если команда уже
// указывает на этот же путь — выходим сразу (вызывается на каждом запуске GUI).
func installAssociation() {
	exe, err := os.Executable()
	if err != nil {
		return
	}
	cmd := `"` + exe + `" "%1"`

	if k, err := registry.OpenKey(registry.CURRENT_USER,
		`Software\Classes\`+progID+`\shell\open\command`, registry.QUERY_VALUE); err == nil {
		v, _, e := k.GetStringValue("")
		k.Close()
		if e == nil && v == cmd {
			return // уже прописано на этот exe
		}
	}

	writes := []struct{ path, name, value string }{
		{`Software\Classes\` + progID, "", progIDTitle},
		{`Software\Classes\` + progID + `\DefaultIcon`, "", exe + ",0"},
		{`Software\Classes\` + progID + `\shell\open\command`, "", cmd},
		{`Software\Classes\.rdp\OpenWithProgids`, progID, ""},
		{`Software\Classes\Applications\rdpkey.exe\shell\open\command`, "", cmd},
		{`Software\Classes\Applications\rdpkey.exe`, "FriendlyAppName", friendlyName},
	}
	for _, w := range writes {
		_ = regSetString(w.path, w.name, w.value)
	}
	shChangeNotifyAssoc()
}
