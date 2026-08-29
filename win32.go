package main

import (
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	user32   = windows.NewLazySystemDLL("user32.dll")
	kernel32 = windows.NewLazySystemDLL("kernel32.dll")
	gdi32    = windows.NewLazySystemDLL("gdi32.dll")
	comctl32 = windows.NewLazySystemDLL("comctl32.dll")
	comdlg32 = windows.NewLazySystemDLL("comdlg32.dll")
	shell32  = windows.NewLazySystemDLL("shell32.dll")

	procSetWindowsHookExW       = user32.NewProc("SetWindowsHookExW")
	procUnhookWindowsHookEx     = user32.NewProc("UnhookWindowsHookEx")
	procCallNextHookEx          = user32.NewProc("CallNextHookEx")
	procGetMessageW             = user32.NewProc("GetMessageW")
	procTranslateMessage        = user32.NewProc("TranslateMessage")
	procDispatchMessageW        = user32.NewProc("DispatchMessageW")
	procPostMessageW            = user32.NewProc("PostMessageW")
	procSendMessageW            = user32.NewProc("SendMessageW")
	procPostQuitMessage         = user32.NewProc("PostQuitMessage")
	procGetForegroundWindow     = user32.NewProc("GetForegroundWindow")
	procGetWindowThreadProcessId = user32.NewProc("GetWindowThreadProcessId")
	procGetClassNameW           = user32.NewProc("GetClassNameW")
	procGetWindowRect           = user32.NewProc("GetWindowRect")
	procMonitorFromWindow       = user32.NewProc("MonitorFromWindow")
	procGetMonitorInfoW         = user32.NewProc("GetMonitorInfoW")
	procEnumWindows             = user32.NewProc("EnumWindows")
	procEnumChildWindows        = user32.NewProc("EnumChildWindows")
	procIsWindow                = user32.NewProc("IsWindow")
	procIsWindowVisible         = user32.NewProc("IsWindowVisible")
	procGetAsyncKeyState        = user32.NewProc("GetAsyncKeyState")
	procSetTimer                = user32.NewProc("SetTimer")
	procMessageBoxW             = user32.NewProc("MessageBoxW")
	procRegisterClassExW        = user32.NewProc("RegisterClassExW")
	procCreateWindowExW         = user32.NewProc("CreateWindowExW")
	procDefWindowProcW          = user32.NewProc("DefWindowProcW")
	procDestroyWindow           = user32.NewProc("DestroyWindow")
	procShowWindow              = user32.NewProc("ShowWindow")
	procShowWindowAsync         = user32.NewProc("ShowWindowAsync")
	procUpdateWindow            = user32.NewProc("UpdateWindow")
	procInvalidateRect          = user32.NewProc("InvalidateRect")
	procLoadCursorW             = user32.NewProc("LoadCursorW")
	procLoadIconW               = user32.NewProc("LoadIconW")
	procGetSystemMetrics        = user32.NewProc("GetSystemMetrics")
	procEnableWindow            = user32.NewProc("EnableWindow")
	procSetWindowTextW          = user32.NewProc("SetWindowTextW")
	procGetDpiForWindow         = user32.NewProc("GetDpiForWindow")
	procSetProcessDpiAwarenessContext = user32.NewProc("SetProcessDpiAwarenessContext")
	procMoveWindow              = user32.NewProc("MoveWindow")
	procGetSysColorBrush        = user32.NewProc("GetSysColorBrush")
	procAdjustWindowRectExForDpi = user32.NewProc("AdjustWindowRectExForDpi")
	procGetDC                   = user32.NewProc("GetDC")
	procReleaseDC               = user32.NewProc("ReleaseDC")

	procGetCurrentProcessId = kernel32.NewProc("GetCurrentProcessId")
	procGetModuleHandleW    = kernel32.NewProc("GetModuleHandleW")
	procGetTickCount64      = kernel32.NewProc("GetTickCount64")

	procCreateFontW           = gdi32.NewProc("CreateFontW")
	procSetBkMode             = gdi32.NewProc("SetBkMode")
	procSetTextColor          = gdi32.NewProc("SetTextColor")
	procSelectObject          = gdi32.NewProc("SelectObject")
	procGetTextExtentPoint32W = gdi32.NewProc("GetTextExtentPoint32W")

	procInitCommonControlsEx = comctl32.NewProc("InitCommonControlsEx")

	procGetOpenFileNameW = comdlg32.NewProc("GetOpenFileNameW")

	procSHChangeNotify  = shell32.NewProc("SHChangeNotify")
	procSHOpenWithDialog = shell32.NewProc("SHOpenWithDialog")
	procShellExecuteW   = shell32.NewProc("ShellExecuteW")
)

const (
	WH_KEYBOARD_LL = 13
	HC_ACTION      = 0

	WM_KEYDOWN    = 0x0100
	WM_KEYUP      = 0x0101
	WM_SYSKEYDOWN = 0x0104
	WM_SYSKEYUP   = 0x0105
	WM_TIMER      = 0x0113
	WM_COMMAND    = 0x0111
	WM_CREATE     = 0x0001
	WM_DESTROY    = 0x0002
	WM_CLOSE      = 0x0010
	WM_SETFONT    = 0x0030
	WM_CTLCOLORSTATIC = 0x0138
	WM_CTLCOLORBTN    = 0x0135

	CW_USEDEFAULT = 0x80000000
	TRANSPARENT   = 1

	WS_POPUP = 0x80000000

	SS_NOTIFY      = 0x00000100
	SS_ENDELLIPSIS = 0x00004000

	// Тултип (comctl32).
	TTS_ALWAYSTIP      = 0x01
	TTS_NOPREFIX       = 0x02
	TTF_IDISHWND       = 0x0001
	TTF_SUBCLASS       = 0x0010
	TTM_ACTIVATE       = 0x0401 // WM_USER+1
	TTM_SETMAXTIPWIDTH = 0x0418 // WM_USER+24
	TTM_ADDTOOLW       = 0x0432 // WM_USER+50
	TTM_UPDATETIPTEXTW = 0x0439 // WM_USER+57

	LLKHF_EXTENDED = 0x01
	LLKHF_INJECTED = 0x10

	VK_TAB      = 0x09
	VK_SHIFT    = 0x10
	VK_CONTROL  = 0x11
	VK_MENU     = 0x12
	VK_ESCAPE   = 0x1B
	VK_LWIN     = 0x5B
	VK_RWIN     = 0x5C
	VK_LSHIFT   = 0xA0
	VK_RSHIFT   = 0xA1
	VK_LCONTROL = 0xA2
	VK_RCONTROL = 0xA3
	VK_LMENU    = 0xA4
	VK_RMENU    = 0xA5

	MONITOR_DEFAULTTONEAREST = 2

	BM_SETCHECK   = 0x00F1
	BM_GETCHECK   = 0x00F0
	BST_CHECKED   = 1
	BST_UNCHECKED = 0

	BS_AUTOCHECKBOX = 0x03
	BS_PUSHBUTTON   = 0x00

	WS_CHILD        = 0x40000000
	WS_VISIBLE      = 0x10000000
	WS_TABSTOP      = 0x00010000
	WS_OVERLAPPED   = 0x00000000
	WS_CAPTION      = 0x00C00000
	WS_SYSMENU      = 0x00080000
	WS_MINIMIZEBOX  = 0x00020000
	WS_OVERLAPPEDWINDOW = 0x00CF0000

	SW_SHOW     = 5
	SW_MINIMIZE = 6

	SM_CXSCREEN = 0
	SM_CYSCREEN = 1

	IDC_ARROW = 32512
	COLOR_BTNFACE = 15

	MB_ICONERROR    = 0x10
	MB_ICONQUESTION = 0x20
	MB_YESNO        = 0x04
	IDYES           = 6

	ICC_STANDARD_CLASSES = 0x00004000
	ICC_WIN95_CLASSES    = 0x000000FF

	OFN_FILEMUSTEXIST = 0x00001000
	OFN_PATHMUSTEXIST = 0x00000800
	OFN_HIDEREADONLY  = 0x00000004
	OFN_EXPLORER      = 0x00080000

	SHCNE_ASSOCCHANGED = 0x08000000
	SHCNF_IDLIST       = 0x0000

	OAIF_ALLOW_REGISTRATION = 0x00000001
	OAIF_REGISTER_EXT       = 0x00000002
	OAIF_EXEC               = 0x00000004

	FW_NORMAL      = 400
	DEFAULT_CHARSET = 1
	DEFAULT_QUALITY = 0
	VARIABLE_PITCH  = 2
)

type KBDLLHOOKSTRUCT struct {
	VkCode      uint32
	ScanCode    uint32
	Flags       uint32
	Time        uint32
	DwExtraInfo uintptr
}

type RECT struct {
	Left, Top, Right, Bottom int32
}

type MONITORINFO struct {
	CbSize    uint32
	RcMonitor RECT
	RcWork    RECT
	DwFlags   uint32
}

type POINT struct {
	X, Y int32
}

type SIZE struct {
	Cx, Cy int32
}

type TOOLINFO struct {
	CbSize     uint32
	UFlags     uint32
	Hwnd       uintptr
	UId        uintptr
	Rect       RECT
	Hinst      uintptr
	LpszText   *uint16
	LParam     uintptr
	LpReserved uintptr
}

type MSG struct {
	Hwnd    uintptr
	Message uint32
	WParam  uintptr
	LParam  uintptr
	Time    uint32
	Pt      POINT
}

type WNDCLASSEX struct {
	CbSize        uint32
	Style         uint32
	LpfnWndProc   uintptr
	CbClsExtra    int32
	CbWndExtra    int32
	HInstance     uintptr
	HIcon         uintptr
	HCursor       uintptr
	HbrBackground uintptr
	LpszMenuName  *uint16
	LpszClassName *uint16
	HIconSm       uintptr
}

type INITCOMMONCONTROLSEX struct {
	DwSize uint32
	DwICC  uint32
}

type OPENFILENAME struct {
	LStructSize       uint32
	HwndOwner         uintptr
	HInstance         uintptr
	LpstrFilter       *uint16
	LpstrCustomFilter *uint16
	NMaxCustFilter    uint32
	NFilterIndex      uint32
	LpstrFile         *uint16
	NMaxFile          uint32
	LpstrFileTitle    *uint16
	NMaxFileTitle     uint32
	LpstrInitialDir   *uint16
	LpstrTitle        *uint16
	Flags             uint32
	NFileOffset       uint16
	NFileExtension    uint16
	LpstrDefExt       *uint16
	LCustData         uintptr
	LpfnHook          uintptr
	LpTemplateName    *uint16
	PvReserved        uintptr
	DwReserved        uint32
	FlagsEx           uint32
}

type OPENASINFO struct {
	PcszFile  *uint16
	PcszClass *uint16
	OaifFlags uint32
}

// ---- тонкие обёртки ----

func getCurrentProcessId() uint32 {
	r, _, _ := procGetCurrentProcessId.Call()
	return uint32(r)
}

func getModuleHandle() uintptr {
	r, _, _ := procGetModuleHandleW.Call(0)
	return r
}

func getTickCount64() uint64 {
	r, _, _ := procGetTickCount64.Call()
	return uint64(r)
}

func isDown(vk int) bool {
	r, _, _ := procGetAsyncKeyState.Call(uintptr(vk))
	return r&0x8000 != 0
}

func getForegroundWindow() uintptr {
	r, _, _ := procGetForegroundWindow.Call()
	return r
}

func getWindowThreadProcessId(hwnd uintptr) uint32 {
	var pid uint32
	procGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
	return pid
}

func isWindow(hwnd uintptr) bool {
	r, _, _ := procIsWindow.Call(hwnd)
	return r != 0
}

func isWindowVisible(hwnd uintptr) bool {
	r, _, _ := procIsWindowVisible.Call(hwnd)
	return r != 0
}

func classNameEqual(hwnd uintptr, want string) bool {
	var buf [64]uint16
	r, _, _ := procGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	n := int(r)
	if n <= 0 {
		return false
	}
	return strings.EqualFold(windows.UTF16ToString(buf[:n]), want)
}

func getWindowRect(hwnd uintptr) (RECT, bool) {
	var r RECT
	ret, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&r)))
	return r, ret != 0
}

func monitorFromWindow(hwnd uintptr) uintptr {
	r, _, _ := procMonitorFromWindow.Call(hwnd, MONITOR_DEFAULTTONEAREST)
	return r
}

func getMonitorInfo(mon uintptr) (MONITORINFO, bool) {
	var mi MONITORINFO
	mi.CbSize = uint32(unsafe.Sizeof(mi))
	r, _, _ := procGetMonitorInfoW.Call(mon, uintptr(unsafe.Pointer(&mi)))
	return mi, r != 0
}

func postMessage(hwnd uintptr, msg uint32, wParam, lParam uintptr) {
	procPostMessageW.Call(hwnd, uintptr(msg), wParam, lParam)
}

func sendMessage(hwnd uintptr, msg uint32, wParam, lParam uintptr) uintptr {
	r, _, _ := procSendMessageW.Call(hwnd, uintptr(msg), wParam, lParam)
	return r
}

func postQuitMessage(code int) {
	procPostQuitMessage.Call(uintptr(code))
}

func getSystemMetrics(index int) int {
	r, _, _ := procGetSystemMetrics.Call(uintptr(index))
	return int(int32(r))
}

func messageBox(text, caption string, flags uint32) int {
	t, _ := windows.UTF16PtrFromString(text)
	c, _ := windows.UTF16PtrFromString(caption)
	r, _, _ := procMessageBoxW.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), uintptr(flags))
	return int(r)
}

func shChangeNotifyAssoc() {
	procSHChangeNotify.Call(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0)
}

// measureTextW: ширина строки в пикселях при заданном шрифте (экранный DC).
func measureTextW(font uintptr, s string) int32 {
	hdc, _, _ := procGetDC.Call(0)
	if hdc == 0 {
		return 0
	}
	defer procReleaseDC.Call(0, hdc)
	old, _, _ := procSelectObject.Call(hdc, font)
	defer procSelectObject.Call(hdc, old)
	u, _ := windows.UTF16FromString(s)
	n := len(u)
	if n > 0 && u[n-1] == 0 {
		n-- // без завершающего нуля
	}
	var sz SIZE
	if n == 0 {
		return 0
	}
	procGetTextExtentPoint32W.Call(hdc, uintptr(unsafe.Pointer(&u[0])), uintptr(n), uintptr(unsafe.Pointer(&sz)))
	return sz.Cx
}
