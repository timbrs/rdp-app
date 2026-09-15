package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

// giveUpTicks: сколько тиков таймера (по 2 с) выждать перед выходом, когда наш mstsc
// уже завершился, новых mstsc не осталось и ни одной сессии так и не появилось.
const giveUpTicks = 5

// baselineRail: HWND'ы всех RAIL_WINDOW, существовавших ДО нашего запуска. Окна из
// набора — чужие сессии, жившие до нас; своими их не считаем ни для живучести, ни
// для привязки хука, иначе rdpkey висел бы, пока открыт хоть один сторонний
// RemoteApp. Ключ — HWND, а не pid: ферма/брокер (mstsc.exe -Embedding) держит окно
// на заранее запущенном процессе, и по pid наш свежий сеанс от чужого не отличить.
var baselineRail map[uintptr]struct{}

var (
	railSnap    map[uintptr]struct{}
	fsRailFound uintptr
)

// enumCollectRail: собрать HWND всех верхних RAIL_WINDOW (любой видимости).
func enumCollectRail(h uintptr, _ uintptr) uintptr {
	if classNameEqual(h, "RAIL_WINDOW") {
		railSnap[h] = struct{}{}
	}
	return 1
}

func railWindows() map[uintptr]struct{} {
	railSnap = map[uintptr]struct{}{}
	procEnumWindows.Call(cbCollectRail, 0)
	return railSnap
}

// isOurRail: окно h не из baseline — появилось после нашего запуска. Общий критерий
// «своего» RAIL-окна для сторожа (session.go) и хука (hook.go).
func isOurRail(h uintptr) bool {
	_, existed := baselineRail[h]
	return !existed
}

// enumFindFsRail: первое видимое полноэкранное RAIL-окно нашей сессии (isOurRail).
func enumFindFsRail(h uintptr, _ uintptr) uintptr {
	if !isWindowVisible(h) || !classNameEqual(h, "RAIL_WINDOW") {
		return 1
	}
	if !isOurRail(h) {
		return 1
	}
	if !isWindowFullScreen(h) {
		return 1
	}
	fsRailFound = h
	return 0
}

func findFullscreenRail() uintptr {
	fsRailFound = 0
	procEnumWindows.Call(cbFindFsRail, 0)
	return fsRailFound
}

// mstscPIDs: PID'ы всех запущенных сейчас mstsc.exe.
func mstscPIDs() map[uint32]struct{} {
	out := map[uint32]struct{}{}
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return out
	}
	defer windows.CloseHandle(snap)
	var pe windows.ProcessEntry32
	pe.Size = uint32(unsafe.Sizeof(pe))
	if windows.Process32First(snap, &pe) != nil {
		return out
	}
	for {
		if strings.EqualFold(windows.UTF16ToString(pe.ExeFile[:]), "mstsc.exe") {
			out[pe.ProcessID] = struct{}{}
		}
		if windows.Process32Next(snap, &pe) != nil {
			break
		}
	}
	return out
}

// newMstscSet: mstsc-процессы, появившиеся ПОСЛЕ нашего запуска (текущие минус
// baseline). Чужие/ферменные сессии, жившие до нас, своими не считаем — иначе
// rdpkey висел бы, пока открыт хоть один сторонний RemoteApp.
func newMstscSet(baseline map[uint32]struct{}) map[uint32]struct{} {
	cur := mstscPIDs()
	for pid := range baseline {
		delete(cur, pid)
	}
	return cur
}

// sessionConnecting: сеанс ещё поднимается (ждём логина). Пока наш mstsc жив —
// точно да. После его выхода — да, только пока остался хоть один mstsc, появившийся
// после нас (брокер, которому наш mstsc мог передать сеанс). Проверку по exited
// держим первой: она надёжно закрывает наш собственный pid, поэтому его случайное
// переиспользование в newMstscSet живой логин не оборвёт.
func sessionConnecting(baseline map[uint32]struct{}, exited <-chan struct{}) bool {
	select {
	case <-exited:
		return len(newMstscSet(baseline)) > 0
	default:
		return true
	}
}

func mstscPath() string {
	win := os.Getenv("windir")
	if win == "" {
		win = `C:\Windows`
	}
	return filepath.Join(win, "System32", "mstsc.exe")
}

// runSession: поднимает mstsc, ставит LL-хук, крутит цикл сообщений со сторожем
// живучести. GUI к этому моменту уже разрушен — процесс живёт скрыто и завершается
// вместе с сеансом.
func runSession(rdp string, hk Hotkeys) {
	gHotkeys = hk

	// Проверка персонального сертификата — при ЛЮБОМ подключении, в т.ч. запуск
	// по ассоциации (двойной клик .rdp), когда GUI не открывается.
	warnIfPersonalCertExpiring()

	mstsc := mstscPath()
	baseline := mstscPIDs()      // снимок mstsc ДО запуска — см. newMstscSet
	baselineRail = railWindows() // снимок RAIL-окон ДО запуска — см. baselineRail

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
	// Реапим mstsc и заодно узнаём момент его выхода (для фазы ожидания логина).
	exited := make(chan struct{})
	go func() { cmd.Wait(); close(exited) }()

	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	hookHandle, _, _ = procSetWindowsHookExW.Call(WH_KEYBOARD_LL, cbHookProc, getModuleHandle(), 0)
	if hookHandle == 0 {
		messageBox("Не удалось установить LL-хук.", "rdpkey", MB_ICONERROR)
		return
	}

	procSetTimer.Call(0, 1, 2000, 0)
	seen := false // видели ли хоть раз живую полноэкранную сессию
	gone := 0
	var msg MSG
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
		if int32(r) <= 0 { // 0 = WM_QUIT, -1 = ошибка
			break
		}
		if msg.Message == WM_TIMER {
			// Якорь — RAIL-окно нашей сессии (его же ставит хук при форварде). Пока
			// окно живо (в т.ч. свёрнуто) — сессия жива. Умерло — пробуем
			// перепривязаться к свежему полноэкранному RAIL (пересоздание/переподкл.).
			// Полный обход окон делаем только когда якоря нет, а перебор процессов
			// (внутри sessionConnecting) — только в фазе ожидания логина, не каждый тик.
			if boundRail == 0 || !isWindow(boundRail) {
				boundRail = findFullscreenRail()
			}

			switch {
			case boundRail != 0:
				seen = true
				gone = 0
			case seen:
				gone++
				if gone >= 2 { // сессия была и пропала
					postQuitMessage(0)
				}
			default:
				// Сессии ещё не было: ждём логина (может тянуться 5+ минут).
				if sessionConnecting(baseline, exited) {
					gone = 0
				} else {
					gone++
					if gone >= giveUpTicks {
						postQuitMessage(0)
					}
				}
			}
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&msg)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
	}
	procUnhookWindowsHookEx.Call(hookHandle)
}
