package main

import (
	"encoding/json"
	"os"
	"path/filepath"
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
	WinOther    bool `json:"winOther"`
}

type Config struct {
	Hotkeys     Hotkeys `json:"hotkeys"`
	LastRdpFile string  `json:"lastRdpFile"`
}

func defaultConfig() Config {
	return Config{
		Hotkeys: Hotkeys{
			Win: true, CtrlEsc: true, AltTab: true, AltShiftTab: true,
			WinR: true, WinE: true, WinD: true, WinTab: true, WinV: true, WinZ: true, WinOther: true,
		},
	}
}

func appDataDir() string {
	return filepath.Join(os.Getenv("LOCALAPPDATA"), "rdpkey")
}

func configPath() string {
	return filepath.Join(appDataDir(), "config.json")
}

// Отсутствующий/битый файл -> дефолты (все хоткеи включены).
func loadConfig() Config {
	cfg := defaultConfig()
	data, err := os.ReadFile(configPath())
	if err != nil {
		return cfg
	}
	_ = json.Unmarshal(data, &cfg)
	return cfg
}

// Атомарная запись: temp в той же папке + rename.
func saveConfig(cfg Config) error {
	dir := appDataDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
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
