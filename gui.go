package main

import (
	"path/filepath"
	"runtime"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

const (
	idLaunch       = 100
	idAssocChk     = 102
	idUpdate       = 103
	idHelp         = 104
	idOpenDefaults = 105
	idLastFile     = 300
)

type hkItem struct {
	id     int
	label  string
	field  *bool
	indent bool
	winSub bool // деактивируется при снятой мастер-галке «Win»
	master bool
	hwnd   uintptr
}

var (
	gCfgPtr    *Config
	gGuiResult string
	gWndStyle  uint32
	gDPI       int
	gAssocRect RECT
	hMainWnd   uintptr
	hLastFile  uintptr
	hAssocChk  uintptr
	faceBrush  uintptr
	bigFont    uintptr
	guiFont    uintptr

	hkItems []*hkItem
	hkByID  = map[int]*hkItem{}

	cbWndProc = syscall.NewCallback(wndProc)
)

func buildHkItems(cfg *Config) {
	h := &cfg.Hotkeys
	hkItems = []*hkItem{
		{id: 200, label: "Win — открыть «Пуск» в сессии", field: &h.Win, master: true},
		{id: 201, label: "Win+R — «Выполнить»", field: &h.WinR, indent: true, winSub: true},
		{id: 202, label: "Win+E — Проводник", field: &h.WinE, indent: true, winSub: true},
		{id: 203, label: "Win+D — показать рабочий стол", field: &h.WinD, indent: true, winSub: true},
		{id: 204, label: "Win+Tab — представление задач", field: &h.WinTab, indent: true, winSub: true},
		{id: 205, label: "Win+V — журнал буфера обмена", field: &h.WinV, indent: true, winSub: true},
		{id: 210, label: "Win+Z — свернуть удалёнку (локально)", field: &h.WinZ, indent: true, winSub: true},
		{id: 206, label: "Win+<прочие клавиши>", field: &h.WinOther, indent: true, winSub: true},
		{id: 207, label: "Ctrl+Esc — открыть «Пуск»", field: &h.CtrlEsc},
		{id: 208, label: "Alt+Tab — переключение окон", field: &h.AltTab},
		{id: 209, label: "Alt+Shift+Tab — переключение назад", field: &h.AltShiftTab},
	}
	hkByID = map[int]*hkItem{}
	for _, it := range hkItems {
		hkByID[it.id] = it
	}
}

func initDPIAware() {
	if procSetProcessDpiAwarenessContext.Find() == nil {
		procSetProcessDpiAwarenessContext.Call(^uintptr(3)) // PER_MONITOR_AWARE_V2 (-4)
	}
}

func initCommonControls() {
	icc := INITCOMMONCONTROLSEX{
		DwSize: uint32(unsafe.Sizeof(INITCOMMONCONTROLSEX{})),
		DwICC:  ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES,
	}
	procInitCommonControlsEx.Call(uintptr(unsafe.Pointer(&icc)))
}

func dpiForWindow(hwnd uintptr) int {
	if procGetDpiForWindow.Find() == nil {
		r, _, _ := procGetDpiForWindow.Call(hwnd)
		if r != 0 {
			return int(r)
		}
	}
	return 96
}

func makeFont(pt, weight, dpi int) uintptr {
	height := -(pt * dpi / 72)
	face, _ := windows.UTF16PtrFromString("Segoe UI")
	r, _, _ := procCreateFontW.Call(
		uintptr(int32(height)), 0, 0, 0, uintptr(weight), 0, 0, 0,
		DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, VARIABLE_PITCH,
		uintptr(unsafe.Pointer(face)))
	return r
}

func createChild(parent uintptr, class, text string, style uint32, x, y, w, h int32, id int, font uintptr) uintptr {
	cls, _ := windows.UTF16PtrFromString(class)
	txt, _ := windows.UTF16PtrFromString(text)
	hwnd, _, _ := procCreateWindowExW.Call(
		0, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(txt)),
		uintptr(style|WS_CHILD|WS_VISIBLE),
		uintptr(x), uintptr(y), uintptr(w), uintptr(h),
		parent, uintptr(id), getModuleHandle(), 0)
	if font != 0 {
		sendMessage(hwnd, WM_SETFONT, font, 1)
	}
	return hwnd
}

func checkVal(b bool) uintptr {
	if b {
		return BST_CHECKED
	}
	return BST_UNCHECKED
}

func lastFileLabel() string {
	if gCfgPtr.LastRdpFile == "" {
		return "Последний файл: —"
	}
	return "Последний файл: " + filepath.Base(gCfgPtr.LastRdpFile)
}

func updateWinSubEnable() {
	master := hkByID[200]
	on := *master.field
	for _, it := range hkItems {
		if it.winSub {
			en := uintptr(0)
			if on {
				en = 1
			}
			procEnableWindow.Call(it.hwnd, en)
		}
	}
}

func adjustOuter(clientW, clientH int32, style uint32, dpi int) (int32, int32) {
	if procAdjustWindowRectExForDpi.Find() == nil {
		r := RECT{0, 0, clientW, clientH}
		ret, _, _ := procAdjustWindowRectExForDpi.Call(
			uintptr(unsafe.Pointer(&r)), uintptr(style), 0, 0, uintptr(dpi))
		if ret != 0 {
			return r.Right - r.Left, r.Bottom - r.Top
		}
	}
	return clientW + 16, clientH + 39 // грубая оценка рамки, если API нет
}

func onCreate(hwnd uintptr) {
	dpi := dpiForWindow(hwnd)
	gDPI = dpi
	sc := func(v int) int32 { return int32(v * dpi / 96) }
	bigFont = makeFont(14, 600, dpi)
	guiFont = makeFont(9, FW_NORMAL, dpi)

	const margin = 20
	const btnW = 420

	// Верхняя строка: версия слева, «Проверить обновления» справа.
	createChild(hwnd, "STATIC", "rdpkey v"+appVersion, 0,
		sc(margin), sc(15), sc(180), sc(20), 0, guiFont)
	const updW = 175
	createChild(hwnd, "BUTTON", "Проверить обновления",
		BS_PUSHBUTTON|WS_TABSTOP, sc(margin+btnW-updW), sc(11), sc(updW), sc(26), idUpdate, guiFont)

	// Большая кнопка запуска.
	createChild(hwnd, "BUTTON", "Запустить удалёнку с пробросом клавиш",
		BS_PUSHBUTTON|WS_TABSTOP, sc(margin), sc(46), sc(btnW), sc(56), idLaunch, bigFont)

	// Ассоциация: чекбокс в синей рамке + кнопка к системным настройкам.
	gAssocRect = RECT{sc(margin), sc(110), sc(margin + btnW), sc(142)}
	hAssocChk = createChild(hwnd, "BUTTON", "Всегда открывать удалёнку с пробросом",
		BS_AUTOCHECKBOX|WS_TABSTOP, sc(margin+14), sc(118), sc(btnW-28), sc(20), idAssocChk, guiFont)
	sendMessage(hAssocChk, BM_SETCHECK, checkVal(associationRegistered()), 0)

	createChild(hwnd, "BUTTON", "Открыть «Приложения по умолчанию» Windows",
		BS_PUSHBUTTON|WS_TABSTOP, sc(margin), sc(150), sc(320), sc(26), idOpenDefaults, guiFont)

	hLastFile = createChild(hwnd, "STATIC", lastFileLabel(), 0,
		sc(margin), sc(186), sc(btnW), sc(20), idLastFile, guiFont)

	y := 214
	for _, it := range hkItems {
		x := margin
		w := btnW
		if it.indent {
			x += 22
			w -= 22
		}
		it.hwnd = createChild(hwnd, "BUTTON", it.label,
			BS_AUTOCHECKBOX|WS_TABSTOP, sc(x), sc(y), sc(w), sc(22), it.id, guiFont)
		sendMessage(it.hwnd, BM_SETCHECK, checkVal(*it.field), 0)
		y += 25
	}
	y += 6
	createChild(hwnd, "BUTTON", "Как пользоваться?",
		BS_PUSHBUTTON|WS_TABSTOP, sc(margin), sc(y), sc(200), sc(26), idHelp, guiFont)
	clientH := y + 26 + 14

	updateWinSubEnable()

	ow, oh := adjustOuter(sc(margin*2+btnW), sc(clientH), gWndStyle, dpi)
	scrW := int32(getSystemMetrics(SM_CXSCREEN))
	scrH := int32(getSystemMetrics(SM_CYSCREEN))
	procMoveWindow.Call(hwnd, uintptr((scrW-ow)/2), uintptr((scrH-oh)/2), uintptr(ow), uintptr(oh), 1)
}

func utf16Filter(parts ...string) []uint16 {
	var buf []uint16
	for _, p := range parts {
		u, _ := windows.UTF16FromString(p)
		buf = append(buf, u...)
	}
	return append(buf, 0)
}

func openRdpDialog(hwnd uintptr, initial string) (string, bool) {
	buf := make([]uint16, 1024)
	if initial != "" {
		if u, err := windows.UTF16FromString(initial); err == nil {
			copy(buf, u)
		}
	}
	filter := utf16Filter("RDP-файлы (*.rdp)", "*.rdp", "Все файлы (*.*)", "*.*")
	title, _ := windows.UTF16PtrFromString("Выберите .rdp-файл")
	defExt, _ := windows.UTF16PtrFromString("rdp")
	var initDir *uint16
	if initial != "" {
		initDir, _ = windows.UTF16PtrFromString(filepath.Dir(initial))
	}
	ofn := OPENFILENAME{
		LStructSize:     uint32(unsafe.Sizeof(OPENFILENAME{})),
		HwndOwner:       hwnd,
		LpstrFilter:     &filter[0],
		LpstrFile:       &buf[0],
		NMaxFile:        uint32(len(buf)),
		LpstrTitle:      title,
		LpstrInitialDir: initDir,
		LpstrDefExt:     defExt,
		Flags:           OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER,
	}
	r, _, _ := procGetOpenFileNameW.Call(uintptr(unsafe.Pointer(&ofn)))
	if r == 0 {
		return "", false
	}
	return windows.UTF16ToString(buf), true
}

func onCommand(hwnd, wParam uintptr) {
	id := int(uint16(wParam)) // LOWORD
	switch id {
	case idLaunch:
		if p, ok := openRdpDialog(hwnd, gCfgPtr.LastRdpFile); ok {
			gCfgPtr.LastRdpFile = p
			gGuiResult = p
			saveConfig(*gCfgPtr)
			procDestroyWindow.Call(hwnd)
		}
	case idUpdate:
		openURL(releasesURL)
	case idHelp:
		openHelp()
	case idOpenDefaults:
		openURL("ms-settings:defaultapps")
	case idAssocChk:
		if sendMessage(hAssocChk, BM_GETCHECK, 0, 0) == BST_CHECKED {
			if _, err := installAssociation(); err == nil {
				gCfgPtr.Installed = true
				saveConfig(*gCfgPtr)
			} else {
				sendMessage(hAssocChk, BM_SETCHECK, BST_UNCHECKED, 0)
				messageBox("Не удалось зарегистрировать rdpkey для .rdp.\n\n"+
					"Что попробовать:\n"+
					"• Закрыть другие открытые окна rdpkey и нажать ещё раз.\n"+
					"• Запускать rdpkey с локального диска (не из архива или сетевой папки).\n"+
					"• Проверить доступ к папке %LOCALAPPDATA%\\rdpkey.\n\n"+
					"Подробнее — кнопка «Как пользоваться?».",
					"rdpkey — ошибка регистрации", MB_ICONERROR)
			}
		} else {
			uninstallAssociation()
			gCfgPtr.Installed = false
			saveConfig(*gCfgPtr)
		}
	default:
		if it, ok := hkByID[id]; ok {
			*it.field = sendMessage(it.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED
			if it.master {
				updateWinSubEnable()
			}
			saveConfig(*gCfgPtr)
		}
	}
}

func openURL(url string) {
	verb, _ := windows.UTF16PtrFromString("open")
	u, _ := windows.UTF16PtrFromString(url)
	procShellExecuteW.Call(0, uintptr(unsafe.Pointer(verb)), uintptr(unsafe.Pointer(u)), 0, 0, SW_SHOW)
}

// Синяя скруглённая рамка вокруг чекбокса ассоциации, чтобы он был заметен.
func paintAssocFrame(hwnd uintptr) {
	var ps PAINTSTRUCT
	hdc, _, _ := procBeginPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
	w := gDPI * 2 / 96
	if w < 1 {
		w = 1
	}
	pen, _, _ := procCreatePen.Call(PS_SOLID, uintptr(w), 0x00D77800) // RGB(0,120,215)
	br, _, _ := procGetStockObject.Call(NULL_BRUSH)
	oldPen, _, _ := procSelectObject.Call(hdc, pen)
	oldBr, _, _ := procSelectObject.Call(hdc, br)
	rad := uintptr(gDPI * 8 / 96)
	procRoundRect.Call(hdc, uintptr(gAssocRect.Left), uintptr(gAssocRect.Top),
		uintptr(gAssocRect.Right), uintptr(gAssocRect.Bottom), rad, rad)
	procSelectObject.Call(hdc, oldPen)
	procSelectObject.Call(hdc, oldBr)
	procDeleteObject.Call(pen)
	procEndPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
}

func wndProc(hwnd, msg, wParam, lParam uintptr) uintptr {
	switch uint32(msg) {
	case WM_CREATE:
		onCreate(hwnd)
		return 0
	case WM_COMMAND:
		onCommand(hwnd, wParam)
		return 0
	case WM_PAINT:
		paintAssocFrame(hwnd)
		return 0
	case WM_CTLCOLORSTATIC, WM_CTLCOLORBTN:
		procSetBkMode.Call(wParam, TRANSPARENT)
		return faceBrush
	case WM_CLOSE:
		procDestroyWindow.Call(hwnd)
		return 0
	case WM_DESTROY:
		postQuitMessage(0)
		return 0
	}
	r, _, _ := procDefWindowProcW.Call(hwnd, msg, wParam, lParam)
	return r
}

func runGUI(cfg *Config) string {
	// Окно, цикл сообщений и модальный GetOpenFileNameW обязаны жить на одном
	// OS-потоке; без блокировки Go-планировщик уводит горутину и вложенный
	// модальный цикл диалога вешается наглухо.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	gCfgPtr = cfg
	buildHkItems(cfg)
	initDPIAware()
	initCommonControls()

	hInst := getModuleHandle()
	faceBrush, _, _ = procGetSysColorBrush.Call(COLOR_BTNFACE)

	className, _ := windows.UTF16PtrFromString("RdpKeyMainWnd")
	ico, _, _ := procLoadIconW.Call(hInst, 1) // «1 ICON» из ресурса
	cur, _, _ := procLoadCursorW.Call(0, IDC_ARROW)
	wc := WNDCLASSEX{
		CbSize:        uint32(unsafe.Sizeof(WNDCLASSEX{})),
		LpfnWndProc:   cbWndProc,
		HInstance:     hInst,
		HIcon:         ico,
		HIconSm:       ico,
		HCursor:       cur,
		HbrBackground: COLOR_BTNFACE + 1,
		LpszClassName: className,
	}
	procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))

	gWndStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
	title, _ := windows.UTF16PtrFromString("rdpkey — проброс горячих клавиш")
	hMainWnd, _, _ = procCreateWindowExW.Call(
		0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)),
		uintptr(gWndStyle),
		uintptr(CW_USEDEFAULT), uintptr(CW_USEDEFAULT), 480, 460,
		0, 0, hInst, 0)
	if hMainWnd == 0 {
		return ""
	}
	procShowWindow.Call(hMainWnd, SW_SHOW)
	procUpdateWindow.Call(hMainWnd)

	var msg MSG
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&msg)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
	}
	return gGuiResult
}
