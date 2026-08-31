package main

import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Блокирующее окно ошибки: первые errLockSeconds секунд закрыть нельзя (кнопка
// «Закрыть» неактивна, крестик игнорируется), таймер показывает, сколько
// осталось — чтобы пользователь понимал, а не пугался. Обычное окно (НЕ поверх
// всех окон).
const errLockSeconds = 5

var (
	hErrBtn       uintptr
	hErrCountdown uintptr
	errRemaining  int
	errBody       string
	errWndStyle   uint32
	errDPI        int
	errFaceBrush  uintptr
	cbErrProc     = syscall.NewCallback(errWndProc)
)

func errSc(v int) int32 { return int32(v * errDPI / 96) }

func countdownText(sec int) string {
	if sec <= 0 {
		return ""
	}
	return fmt.Sprintf("Закрыть можно будет через %d с", sec)
}

// measureTextHeight: высота многострочного текста при заданной ширине (перенос
// по словам) в пикселях устройства — чтобы окно точно вмещало весь текст.
func measureTextHeight(hwnd, font uintptr, s string, width int32) int32 {
	hdc, _, _ := procGetDC.Call(hwnd)
	if hdc == 0 {
		return width // маловероятный фолбэк
	}
	defer procReleaseDC.Call(hwnd, hdc)
	old, _, _ := procSelectObject.Call(hdc, font)
	defer procSelectObject.Call(hdc, old)
	u, _ := windows.UTF16FromString(s)
	n := len(u)
	if n > 0 && u[n-1] == 0 {
		n--
	}
	if n == 0 {
		return 0
	}
	rc := RECT{0, 0, width, 0}
	procDrawTextW.Call(hdc, uintptr(unsafe.Pointer(&u[0])), uintptr(n),
		uintptr(unsafe.Pointer(&rc)), uintptr(DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX))
	return rc.Bottom - rc.Top
}

func errOnCreate(hwnd uintptr) {
	errRemaining = errLockSeconds
	errDPI = dpiForWindow(hwnd)
	font := makeFont(10, FW_NORMAL, errDPI)
	errFaceBrush, _, _ = procGetSysColorBrush.Call(COLOR_BTNFACE)

	const w = 520
	sc := errSc
	textX, textY, textW := sc(70), sc(22), sc(w-92)

	// Системная иконка предупреждения.
	icon, _, _ := procLoadIconW.Call(0, IDI_WARNING)
	hIco := createChild(hwnd, "STATIC", "", SS_ICON, sc(22), sc(24), sc(32), sc(32), 0, 0)
	sendMessage(hIco, STM_SETICON, icon, 0)

	// Высота текста под ширину — окно подстраивается, ничего не обрезается.
	textH := measureTextHeight(hwnd, font, errBody, textW)
	createChild(hwnd, "STATIC", errBody, 0, textX, textY, textW, textH, 0, font)

	// Ряд «отсчёт слева / кнопка справа» под текстом.
	rowY := textY + textH + sc(18)
	hErrCountdown = createChild(hwnd, "STATIC", countdownText(errRemaining), 0,
		sc(22), rowY+sc(6), sc(260), sc(20), 0, font)
	const bw = 110
	hErrBtn = createChild(hwnd, "BUTTON", "Закрыть",
		BS_PUSHBUTTON|WS_TABSTOP|WS_DISABLED, sc(w-bw-20), rowY, sc(bw), sc(30), 1, font)

	procSetTimer.Call(hwnd, 1, 1000, 0)

	clientH := rowY + sc(30) + sc(14)
	ow, oh := adjustOuter(sc(w), clientH, errWndStyle, errDPI)
	scrW := int32(getSystemMetrics(SM_CXSCREEN))
	scrH := int32(getSystemMetrics(SM_CYSCREEN))
	procMoveWindow.Call(hwnd, uintptr((scrW-ow)/2), uintptr((scrH-oh)/2), uintptr(ow), uintptr(oh), 1)
	procSetForegroundWindow.Call(hwnd)
}

func errWndProc(hwnd, msg, wParam, lParam uintptr) uintptr {
	switch uint32(msg) {
	case WM_CREATE:
		errOnCreate(hwnd)
		return 0
	case WM_TIMER:
		errRemaining--
		setWindowText(hErrCountdown, countdownText(errRemaining))
		if errRemaining <= 0 {
			procKillTimer.Call(hwnd, 1)
			procEnableWindow.Call(hErrBtn, 1)
		}
		return 0
	case WM_COMMAND:
		if errRemaining <= 0 {
			procDestroyWindow.Call(hwnd)
		}
		return 0
	case WM_CTLCOLORSTATIC:
		procSetBkMode.Call(wParam, TRANSPARENT)
		return errFaceBrush
	case WM_CLOSE:
		// До конца отсчёта закрытие (крестик/Alt+F4) игнорируем.
		if errRemaining <= 0 {
			procDestroyWindow.Call(hwnd)
		}
		return 0
	case WM_DESTROY:
		postQuitMessage(0)
		return 0
	}
	r, _, _ := procDefWindowProcW.Call(hwnd, msg, wParam, lParam)
	return r
}

// showLockedError создаёт окно, крутит собственный цикл сообщений (окно и цикл
// обязаны жить на одном OS-потоке) и возвращается только после закрытия окна.
func showLockedError(title, body string) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	errBody = body
	hInst := getModuleHandle()
	className, _ := windows.UTF16PtrFromString("RdpKeyErrWnd")
	cur, _, _ := procLoadCursorW.Call(0, IDC_ARROW)
	ico, _, _ := procLoadIconW.Call(hInst, 1)
	wc := WNDCLASSEX{
		CbSize:        uint32(unsafe.Sizeof(WNDCLASSEX{})),
		LpfnWndProc:   cbErrProc,
		HInstance:     hInst,
		HIcon:         ico,
		HIconSm:       ico,
		HCursor:       cur,
		HbrBackground: COLOR_BTNFACE + 1,
		LpszClassName: className,
	}
	procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))

	errWndStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU
	title16, _ := windows.UTF16PtrFromString(title)
	hwnd, _, _ := procCreateWindowExW.Call(
		0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title16)),
		uintptr(errWndStyle),
		uintptr(CW_USEDEFAULT), uintptr(CW_USEDEFAULT), 480, 260,
		0, 0, hInst, 0)
	if hwnd == 0 {
		messageBox(body, title, MB_ICONERROR) // фолбэк, если окно не создалось
		return
	}
	procShowWindow.Call(hwnd, SW_SHOW)
	procUpdateWindow.Call(hwnd)

	var msg MSG
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&msg)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
	}
}
