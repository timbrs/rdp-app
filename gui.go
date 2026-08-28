package main

import (
	"os"
	"path/filepath"
	"runtime"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

const (
	idLaunch = 100
	idHelp   = 104
	idChange = 106
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
	hMainWnd   uintptr
	faceBrush  uintptr
	bigFont    uintptr
	guiFont    uintptr

	// Строка «Подключение к: …».
	gHasConn    bool
	hConnStatic uintptr
	hChangeBtn  uintptr
	hTooltip    uintptr
	gExpiryWarn bool
	gConnX      int32
	gConnY      int32
	gConnH      int32
	gTipBuf     []uint16

	hkItems []*hkItem
	hkByID  = map[int]*hkItem{}

	cbWndProc = syscall.NewCallback(wndProc)
)

func scDPI(v int) int32 { return int32(v * gDPI / 96) }

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

func setWindowText(hwnd uintptr, s string) {
	p, _ := windows.UTF16PtrFromString(s)
	procSetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(p)))
}

func fileExists(p string) bool {
	if p == "" {
		return false
	}
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}

func boolTo(b bool) uintptr {
	if b {
		return 1
	}
	return 0
}

func updateWinSubEnable() {
	master := hkByID[200]
	on := *master.field
	for _, it := range hkItems {
		if it.winSub {
			procEnableWindow.Call(it.hwnd, boolTo(on))
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

// connInfo: текст строки подключения, флаг «скоро истекает» и текст подсказки.
// «до ДАТА» добавляется, только если дату удалось извлечь из подписи .rdp.
func connInfo() (text string, warn bool, tip string) {
	text = "Подключение к: " + filepath.Base(gCfgPtr.LastRdpFile)
	if exp, ok := rdpSignatureExpiry(gCfgPtr.LastRdpFile); ok {
		text += ", до " + formatDate(exp)
		if time.Until(exp) <= 30*24*time.Hour {
			warn = true
			tip = "Срок действия файла удалёнки истекает " + formatDate(exp) +
				". После этой даты подключение перестанет работать — запросите новый файл в службе поддержки."
		}
	}
	return
}

func setupTooltip(hwnd uintptr, tip string, warn bool, add bool) {
	if hTooltip == 0 {
		cls, _ := windows.UTF16PtrFromString("tooltips_class32")
		empty, _ := windows.UTF16PtrFromString("")
		hTooltip, _, _ = procCreateWindowExW.Call(
			0, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(empty)),
			uintptr(WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX),
			uintptr(CW_USEDEFAULT), uintptr(CW_USEDEFAULT), uintptr(CW_USEDEFAULT), uintptr(CW_USEDEFAULT),
			hwnd, 0, getModuleHandle(), 0)
	}
	if hTooltip == 0 {
		return
	}
	gTipBuf, _ = windows.UTF16FromString(tip) // держим буфер живым: тултип читает его позже
	ti := TOOLINFO{
		CbSize:   uint32(unsafe.Sizeof(TOOLINFO{})),
		UFlags:   TTF_IDISHWND | TTF_SUBCLASS,
		Hwnd:     hwnd,
		UId:      hConnStatic,
		Hinst:    getModuleHandle(),
		LpszText: &gTipBuf[0],
	}
	msg := uint32(TTM_UPDATETIPTEXTW)
	if add {
		msg = TTM_ADDTOOLW
	}
	sendMessage(hTooltip, msg, 0, uintptr(unsafe.Pointer(&ti)))
	sendMessage(hTooltip, TTM_SETMAXTIPWIDTH, 0, uintptr(scDPI(320)))
	sendMessage(hTooltip, TTM_ACTIVATE, boolTo(warn), 0)
}

func createConnRow(hwnd uintptr) {
	gConnX = scDPI(20)
	gConnY = scDPI(116)
	gConnH = scDPI(20)
	text, warn, tip := connInfo()
	gExpiryWarn = warn
	tw := measureTextW(guiFont, text)
	hConnStatic = createChild(hwnd, "STATIC", text, SS_NOTIFY,
		gConnX, gConnY, tw+scDPI(4), gConnH, 0, guiFont)
	cw := measureTextW(guiFont, "изменить") + scDPI(20)
	hChangeBtn = createChild(hwnd, "BUTTON", "изменить",
		BS_PUSHBUTTON|WS_TABSTOP, gConnX+tw+scDPI(10), gConnY-scDPI(3), cw, scDPI(24), idChange, guiFont)
	setupTooltip(hwnd, tip, warn, true)
}

func applyConnRow(hwnd uintptr) {
	if !gHasConn {
		return
	}
	text, warn, tip := connInfo()
	gExpiryWarn = warn
	setWindowText(hConnStatic, text)
	tw := measureTextW(guiFont, text)
	procMoveWindow.Call(hConnStatic, uintptr(gConnX), uintptr(gConnY), uintptr(tw+scDPI(4)), uintptr(gConnH), 1)
	cw := measureTextW(guiFont, "изменить") + scDPI(20)
	procMoveWindow.Call(hChangeBtn, uintptr(gConnX+tw+scDPI(10)), uintptr(gConnY-scDPI(3)), uintptr(cw), uintptr(scDPI(24)), 1)
	setupTooltip(hwnd, tip, warn, false)
	procInvalidateRect.Call(hConnStatic, 0, 1)
}

func onCreate(hwnd uintptr) {
	gDPI = dpiForWindow(hwnd)
	bigFont = makeFont(14, 600, gDPI)
	guiFont = makeFont(9, FW_NORMAL, gDPI)

	const margin = 20
	const btnW = 420

	createChild(hwnd, "STATIC", "rdpkey v"+appVersion, 0,
		scDPI(margin), scDPI(15), scDPI(180), scDPI(20), 0, guiFont)

	createChild(hwnd, "BUTTON", "Запустить удалёнку с пробросом клавиш",
		BS_PUSHBUTTON|WS_TABSTOP, scDPI(margin), scDPI(46), scDPI(btnW), scDPI(56), idLaunch, bigFont)

	y := 116
	gHasConn = fileExists(gCfgPtr.LastRdpFile)
	if gHasConn {
		createConnRow(hwnd)
		y = 150
	}

	for _, it := range hkItems {
		x := margin
		w := btnW
		if it.indent {
			x += 22
			w -= 22
		}
		it.hwnd = createChild(hwnd, "BUTTON", it.label,
			BS_AUTOCHECKBOX|WS_TABSTOP, scDPI(x), scDPI(y), scDPI(w), scDPI(22), it.id, guiFont)
		sendMessage(it.hwnd, BM_SETCHECK, checkVal(*it.field), 0)
		y += 25
	}
	y += 6
	createChild(hwnd, "BUTTON", "Как пользоваться?",
		BS_PUSHBUTTON|WS_TABSTOP, scDPI(margin), scDPI(y), scDPI(200), scDPI(26), idHelp, guiFont)
	clientH := y + 26 + 14

	updateWinSubEnable()

	ow, oh := adjustOuter(scDPI(margin*2+btnW), scDPI(clientH), gWndStyle, gDPI)
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
	title, _ := windows.UTF16PtrFromString("Выберите файл удаленки от работодателя")
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

// selectRdpFile: диалог выбора + проверка, что это RemoteApp-файл. Иначе ругаемся
// и возвращаем ok=false (ничего не меняем).
func selectRdpFile(hwnd uintptr, initial string) (string, bool) {
	p, ok := openRdpDialog(hwnd, initial)
	if !ok {
		return "", false
	}
	text, err := readRdpText(p)
	if err != nil {
		messageBox("Не удалось прочитать выбранный файл.", "rdpkey", MB_ICONERROR)
		return "", false
	}
	if !isRemoteAppRdp(text) {
		messageBox("Это не файл удалёнки в режиме RemoteApp.\n\n"+
			"Выберите .rdp-файл удалёнки, выданный работодателем.",
			"rdpkey — неподходящий файл", MB_ICONERROR)
		return "", false
	}
	return p, true
}

func launchOrPick(hwnd uintptr) {
	last := gCfgPtr.LastRdpFile
	if fileExists(last) {
		gGuiResult = last
		procDestroyWindow.Call(hwnd)
		return
	}
	if p, ok := selectRdpFile(hwnd, last); ok {
		gCfgPtr.LastRdpFile = p
		gGuiResult = p
		saveConfig(*gCfgPtr)
		procDestroyWindow.Call(hwnd)
	}
}

func onCommand(hwnd, wParam uintptr) {
	id := int(uint16(wParam)) // LOWORD
	switch id {
	case idLaunch:
		launchOrPick(hwnd)
	case idChange:
		if p, ok := selectRdpFile(hwnd, gCfgPtr.LastRdpFile); ok {
			gCfgPtr.LastRdpFile = p
			saveConfig(*gCfgPtr)
			applyConnRow(hwnd)
		}
	case idHelp:
		openHelp()
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

func wndProc(hwnd, msg, wParam, lParam uintptr) uintptr {
	switch uint32(msg) {
	case WM_CREATE:
		onCreate(hwnd)
		return 0
	case WM_COMMAND:
		onCommand(hwnd, wParam)
		return 0
	case WM_CTLCOLORSTATIC:
		procSetBkMode.Call(wParam, TRANSPARENT)
		if gExpiryWarn && lParam == hConnStatic {
			procSetTextColor.Call(wParam, 0x000000FF) // RGB(255,0,0)
		}
		return faceBrush
	case WM_CTLCOLORBTN:
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
