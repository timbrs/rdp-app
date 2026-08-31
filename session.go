package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"unsafe"
)

// Живая (в т.ч. свёрнутая) сессия = видимое RAIL_WINDOW. Процессы mstsc считать
// нельзя: фоновый mstsc.exe -Embedding переживает закрытие и держит скрытые окна.
var visRail int

func countRailProc(h uintptr, _ uintptr) uintptr {
	if !isWindowVisible(h) {
		return 1
	}
	if classNameEqual(h, "RAIL_WINDOW") {
		visRail++
	}
	return 1
}

func visibleRailCount() int {
	visRail = 0
	procEnumWindows.Call(cbCountRail, 0)
	return visRail
}

func mstscPath() string {
	win := os.Getenv("windir")
	if win == "" {
		win = `C:\Windows`
	}
	return filepath.Join(win, "System32", "mstsc.exe")
}

// RunSession: поднимает mstsc, ставит LL-хук, крутит цикл сообщений со сторожем
// живучести. GUI к этому моменту уже разрушен — процесс живёт скрыто и
// завершается вместе с сеансом.
func runSession(rdp string, hk Hotkeys) {
	gHotkeys = hk

	// Проверка персонального сертификата — при ЛЮБОМ подключении, в т.ч. запуск
	// по ассоциации (двойной клик .rdp), когда GUI не открывается.
	warnIfPersonalCertExpiring()

	mstsc := mstscPath()
	var cmd *exec.Cmd
	if rdp != "" {
		cmd = exec.Command(mstsc, rdp)
	} else {
		cmd = exec.Command(mstsc)
	}
	if err := cmd.Start(); err != nil {
		messageBox("Не удалось запустить mstsc.exe.", "rdpkey", MB_ICONERROR)
		return
	}
	go cmd.Wait() // не держим mstsc, но реапим, чтобы не оставлять зомби-хэндл

	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	hookHandle, _, _ = procSetWindowsHookExW.Call(WH_KEYBOARD_LL, cbHookProc, getModuleHandle(), 0)
	if hookHandle == 0 {
		messageBox("Не удалось установить LL-хук.", "rdpkey", MB_ICONERROR)
		return
	}

	procSetTimer.Call(0, 1, 2000, 0)
	start := getTickCount64()
	seen := false
	zero := 0
	var msg MSG
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
		if int32(r) <= 0 { // 0 = WM_QUIT, -1 = ошибка
			break
		}
		if msg.Message == WM_TIMER {
			c := visibleRailCount()
			if c > 0 {
				seen = true
				zero = 0
			} else if seen {
				zero++
				if zero >= 2 {
					postQuitMessage(0)
				}
			} else if getTickCount64()-start > 120000 {
				postQuitMessage(0)
			}
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&msg)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
	}
	procUnhookWindowsHookEx.Call(hookHandle)
}
