package main

import "os"

const appVersion = "1.0.9"

// Аргумент — путь .rdp, если exe запустили с ним (ярлык «rdpkey.exe файл.rdp» или
// вручную настроенная ассоциация). Go уже разобрал кавычки — путь в os.Args[1].
func argAfterProgram() string {
	if len(os.Args) >= 2 {
		return os.Args[1]
	}
	return ""
}

func main() {
	cfg := loadConfig()
	ensureConfig(cfg) // молча создаём config.ini с дефолтами, если его ещё нет

	// Запуск с .rdp в аргументе: сразу сеанс, без GUI.
	if arg := argAfterProgram(); arg != "" {
		runSession(arg, cfg.Hotkeys)
		return
	}

	// Прописываем rdpkey в «Открыть с помощью» для .rdp на ТЕКУЩИЙ exe (без
	// копирования). Идемпотентно: почти бесплатно, если уже прописано.
	installAssociation()

	rdp := runGUI(&cfg)
	if rdp != "" {
		runSession(rdp, cfg.Hotkeys)
	}
}
