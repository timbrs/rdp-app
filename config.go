package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
)

type Hotkeys struct {
	Win         bool `json:"win"`
	CtrlEsc     bool `json:"ctrlEsc"`
	AltTab      bool `json:"altTab"`
	AltShiftTab bool `json:"altShiftTab"`
	WinR        bool `json:"winR"`
	WinE        bool `json:"winE"`
	WinD        bool `json:"winD"`
	WinTab      bool `json:"winTab"`
	WinV        bool `json:"winV"`
	WinZ        bool `json:"winZ"`
	WinShiftS   bool `json:"winShiftS"`
	WinOther    bool `json:"winOther"`
	PrintScreen bool `json:"printScreen"`
}

type Config struct {
	Hotkeys     Hotkeys
	LastRdpFile string
}

func defaultConfig() Config {
	return Config{
		Hotkeys: Hotkeys{
			Win: true, CtrlEsc: true, AltTab: true, AltShiftTab: true,
			WinR: true, WinE: true, WinD: true, WinTab: true, WinV: true, WinZ: true,
			WinShiftS: true, WinOther: true, PrintScreen: true,
		},
	}
}

func appDataDir() string {
	return filepath.Join(os.Getenv("LOCALAPPDATA"), "rdpkey")
}

func configPath() string {
	return filepath.Join(appDataDir(), "config.ini")
}

func legacyJSONPath() string {
	return filepath.Join(appDataDir(), "config.json")
}

// Отсутствующий/битый файл -> дефолты (все хоткеи включены). Формат — INI
// (пути с одиночными \ и переменными %VAR% без экранирования). Старый config.json
// подхватывается один раз для миграции, пока не появился config.ini.
func loadConfig() Config {
	cfg := defaultConfig()
	if data, err := os.ReadFile(configPath()); err == nil {
		applyINI(&cfg, string(data))
		return cfg
	}
	if data, err := os.ReadFile(legacyJSONPath()); err == nil {
		_ = json.Unmarshal(data, &cfg)
	}
	return cfg
}

// ensureConfig молча создаёт config.ini с дефолтами, если файла ещё нет (ошибку
// не показываем — не мешаем запуску). Чтобы у пользователя/девопса всегда был
// готовый файл для правки в %LOCALAPPDATA%\rdpkey\config.ini.
func ensureConfig(cfg Config) {
	if _, err := os.Stat(configPath()); err != nil {
		_ = saveConfig(cfg)
	}
}

// Атомарная запись: temp в той же папке + rename.
func saveConfig(cfg Config) error {
	dir := appDataDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	// UTF-8 BOM байтами (чтобы редакторы верно показывали кириллицу в комментариях;
	// parseINI его отрезает при чтении).
	data := append([]byte{0xEF, 0xBB, 0xBF}, buildINI(cfg)...)
	tmp, err := os.CreateTemp(dir, "config-*.tmp")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		os.Remove(tmpName)
		return err
	}
	if err := tmp.Close(); err != nil {
		os.Remove(tmpName)
		return err
	}
	return os.Rename(tmpName, configPath())
}

// buildINI сериализует конфиг в INI. Ключи хоткеев пишем в camelCase для
// читабельности, читаются они регистронезависимо.
func buildINI(cfg Config) string {
	var b strings.Builder
	b.WriteString("; Настройки rdpkey. Лежит в %LOCALAPPDATA%\\rdpkey\\config.ini\n")
	b.WriteString("; Пути можно писать с одиночными \\ и переменными окружения,\n")
	b.WriteString("; например: lastRdpFile=%USERPROFILE%\\Desktop\\work.rdp\n\n")
	b.WriteString("[hotkeys]\n")
	wb := func(k string, v bool) {
		b.WriteString(k + "=")
		if v {
			b.WriteString("true\n")
		} else {
			b.WriteString("false\n")
		}
	}
	h := cfg.Hotkeys
	wb("win", h.Win)
	wb("ctrlEsc", h.CtrlEsc)
	wb("altTab", h.AltTab)
	wb("altShiftTab", h.AltShiftTab)
	wb("winR", h.WinR)
	wb("winE", h.WinE)
	wb("winD", h.WinD)
	wb("winTab", h.WinTab)
	wb("winV", h.WinV)
	wb("winZ", h.WinZ)
	wb("winShiftS", h.WinShiftS)
	wb("winOther", h.WinOther)
	wb("printScreen", h.PrintScreen)
	b.WriteString("\n[general]\n")
	b.WriteString("lastRdpFile=" + collapseEnv(cfg.LastRdpFile) + "\n")
	return b.String()
}

// underDir: если p лежит внутри dir (по границе разделителя) — вернуть остаток и true.
func underDir(p, dir string) (string, bool) {
	dir = strings.TrimRight(dir, `\/`)
	if dir == "" || len(p) < len(dir) || !strings.EqualFold(p[:len(dir)], dir) {
		return "", false
	}
	rest := p[len(dir):]
	if rest == "" || rest[0] == '\\' || rest[0] == '/' {
		return rest, true
	}
	return "", false
}

// collapseEnv: свернуть абсолютный путь в вид со стандартной переменной окружения,
// чтобы конфиг был переносимым между пользователями. Приоритет — самый длинный
// (специфичный) префикс: %LOCALAPPDATA%/%APPDATA%/%OneDrive%/%USERPROFILE%/%PUBLIC%
// (путь на рабочем столе свернётся в %USERPROFILE%\Desktop\...). Уже свёрнутый путь
// (начинается с %) не трогаем.
func collapseEnv(p string) string {
	if p == "" || strings.HasPrefix(p, "%") {
		return p
	}
	type cand struct{ token, dir string }
	var cs []cand
	for _, n := range []string{"OneDrive", "LOCALAPPDATA", "APPDATA", "USERPROFILE", "PUBLIC"} {
		if v := os.Getenv(n); v != "" {
			cs = append(cs, cand{"%" + n + "%", v})
		}
	}
	bestTok, bestRest, bestLen := "", "", -1
	for _, c := range cs {
		if rest, ok := underDir(p, c.dir); ok && len(c.dir) > bestLen {
			bestTok, bestRest, bestLen = c.token, rest, len(c.dir)
		}
	}
	if bestLen >= 0 {
		return bestTok + bestRest
	}
	return p
}

// applyINI накладывает значения из INI поверх cfg (отсутствующие ключи — как есть).
func applyINI(cfg *Config, text string) {
	m := parseINI(text)
	h := m["hotkeys"]
	set := func(key string, f *bool) {
		if v, ok := h[key]; ok {
			*f = parseBool(v)
		}
	}
	set("win", &cfg.Hotkeys.Win)
	set("ctrlesc", &cfg.Hotkeys.CtrlEsc)
	set("alttab", &cfg.Hotkeys.AltTab)
	set("altshifttab", &cfg.Hotkeys.AltShiftTab)
	set("winr", &cfg.Hotkeys.WinR)
	set("wine", &cfg.Hotkeys.WinE)
	set("wind", &cfg.Hotkeys.WinD)
	set("wintab", &cfg.Hotkeys.WinTab)
	set("winv", &cfg.Hotkeys.WinV)
	set("winz", &cfg.Hotkeys.WinZ)
	set("winshifts", &cfg.Hotkeys.WinShiftS)
	set("winother", &cfg.Hotkeys.WinOther)
	set("printscreen", &cfg.Hotkeys.PrintScreen)
	if g := m["general"]; g != nil {
		if v, ok := g["lastrdpfile"]; ok {
			cfg.LastRdpFile = v
		}
	}
}

// parseINI: минимальный разбор INI -> map[секция][ключ]=значение. Секции и ключи
// в нижнем регистре; комментарии — строки, начинающиеся с ';' или '#'. Значение
// берётся как есть (одиночные '\' и '%VAR%' не трогаем), делится по первому '='.
func parseINI(text string) map[string]map[string]string {
	res := map[string]map[string]string{"": {}}
	section := ""
	if len(text) >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF {
		text = text[3:] // отрезаем возможный UTF-8 BOM
	}
	for _, raw := range strings.Split(text, "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, ";") || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = strings.ToLower(strings.TrimSpace(line[1 : len(line)-1]))
			if res[section] == nil {
				res[section] = map[string]string{}
			}
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			continue
		}
		key := strings.ToLower(strings.TrimSpace(line[:eq]))
		res[section][key] = strings.TrimSpace(line[eq+1:])
	}
	return res
}

func parseBool(s string) bool {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "1", "true", "yes", "on", "да":
		return true
	}
	return false
}
