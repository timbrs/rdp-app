package main

import "os"

const appVersion = "1.0.5"

// Аргумент — путь .rdp, переданный ассоциацией/двойным кликом. Go уже разобрал
// кавычки, так что путь целиком лежит в os.Args[1].
func argAfterProgram() string {
	if len(os.Args) >= 2 {
		return os.Args[1]
	}
	return ""
}

func main() {
	cfg := loadConfig()

	// Запуск через ассоциацию: сразу сеанс, без GUI.
	if arg := argAfterProgram(); arg != "" {
		runSession(arg, cfg.Hotkeys)
		return
	}

	// Открыли GUI: тихо прописываем rdpkey в «Открыть с помощью» для .rdp
	// (не как приложение по умолчанию), чтобы программа была в списке выбора.
	if _, err := installAssociation(); err == nil {
		cfg.Installed = true
		saveConfig(cfg)
	}

	rdp := runGUI(&cfg)
	if rdp != "" {
		runSession(rdp, cfg.Hotkeys)
	}
}
