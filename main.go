package main

import "os"

const appVersion = "1.0.4"

// Ссылка на страницу релизов (легко поменять при смене репозитория).
const releasesURL = "https://github.com/timbrs/rdp-app/releases/latest"

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

	rdp := runGUI(&cfg)
	if rdp != "" {
		runSession(rdp, cfg.Hotkeys)
	}
}
