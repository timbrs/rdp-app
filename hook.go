package main

import (
	"syscall"
	"unsafe"
)

// noSys: слать KEYDOWN даже при зажатом Alt (обход фильтра Alt+Tab в mstscax).
type Scan struct {
	scan  int
	up    bool
	ext   bool
	vk    int
	noSys bool
}

const (
	scLAlt   = 0x38
	scLShift = 0x2A
	scLCtrl  = 0x1D
	scEsc    = 0x01

	winWireVK    = 0x41 // Win: scan 0x5B, но vk 0x41 — обход фильтра Win-клавиши в mstscax
	winHoldMaxMS = 30000
)

// Маски модификаторов. KM_, а не MOD_ — не путать с макросами winuser.h.
const (
	kmAlt   = 1
	kmCtrl  = 2
	kmShift = 4
	kmWin   = 8
)

// Конфиг сеанса. Ставится один раз перед установкой хука, дальше только читается.
var gHotkeys Hotkeys

// Состояние хука. Живёт на единственном потоке хука — синхронизация не нужна.
var (
	winDown     bool
	winCombo    bool
	winScan     = 0x5B
	winExt      = true
	winDownTick uint64

	altSticky   bool
	altScan     = scLAlt
	altExt      bool
	shiftSticky bool
	shiftScan   = scLShift
	shiftExt    bool

	targetPid  uint32
	ihCachePid uint32
	ihCacheWnd uintptr

	hookHandle uintptr
)

func makeLParam(scan int, ext, up, altCtx bool) uintptr {
	var l uintptr = 1 // repeat count = 1, НЕ 0 — иначе mstscax молча игнорирует
	l |= uintptr(scan&0xFF) << 16
	if ext {
		l |= 1 << 24
	}
	if altCtx {
		l |= 1 << 29
	}
	if up {
		l |= (1 << 30) | (1 << 31)
	}
	return l
}

var (
	findPid uint32
	findWnd uintptr

	cbEnumChildIH = syscall.NewCallback(enumChildIH)
	cbEnumTopIH   = syscall.NewCallback(enumTopIH)
	cbCountRail   = syscall.NewCallback(countRailProc)
	cbHookProc    = syscall.NewCallback(hookProc)
)

func enumChildIH(h uintptr, _ uintptr) uintptr {
	if classNameEqual(h, "IHWindowClass") {
		findWnd = h
		return 0
	}
	return 1
}

func enumTopIH(top uintptr, _ uintptr) uintptr {
	if getWindowThreadProcessId(top) == findPid {
		if classNameEqual(top, "IHWindowClass") {
			findWnd = top
			return 0
		}
		procEnumChildWindows.Call(top, cbEnumChildIH, 0)
		if findWnd != 0 {
			return 0
		}
	}
	return 1
}

func findIHWindow(pid uint32) uintptr {
	if pid == 0 {
		return 0
	}
	if ihCachePid == pid && ihCacheWnd != 0 && isWindow(ihCacheWnd) {
		return ihCacheWnd
	}
	findPid = pid
	findWnd = 0
	procEnumWindows.Call(cbEnumTopIH, 0)
	ihCachePid = pid
	ihCacheWnd = findWnd
	return findWnd
}

func postSeq(seq []Scan) {
	t := findIHWindow(targetPid)
	if t == 0 || len(seq) == 0 {
		return
	}
	altDown := isDown(VK_MENU)
	for _, s := range seq {
		// ctx (alt-контекст) считаем на момент отправки.
		ctx := false
		if !s.noSys {
			ctx = altDown || s.vk == VK_MENU
		}
		var msg uint32
		if s.up {
			if ctx {
				msg = WM_SYSKEYUP
			} else {
				msg = WM_KEYUP
			}
		} else {
			if ctx {
				msg = WM_SYSKEYDOWN
			} else {
				msg = WM_KEYDOWN
			}
		}
		postMessage(t, msg, uintptr(s.vk), makeLParam(s.scan, s.ext, s.up, ctx))
	}
}

// Клавиши, участвующие в пробрасываемых сочетаниях. Всё остальное хук
// не рассматривает: выходим до чтения scan-кода и до проверки фокуса.
func isWatchedKey(vk int) bool {
	switch vk {
	case VK_LWIN, VK_RWIN,
		VK_MENU, VK_LMENU, VK_RMENU,
		VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
		VK_TAB, VK_ESCAPE:
		return true
	}
	return false
}

func isModifierKey(vk int) bool {
	switch vk {
	case VK_CONTROL, VK_LCONTROL, VK_RCONTROL:
		return true
	}
	return isWatchedKey(vk) && vk != VK_TAB && vk != VK_ESCAPE
}

func modMask() int {
	m := 0
	if isDown(VK_MENU) {
		m |= kmAlt
	}
	if isDown(VK_CONTROL) {
		m |= kmCtrl
	}
	if isDown(VK_SHIFT) {
		m |= kmShift
	}
	if isDown(VK_LWIN) || isDown(VK_RWIN) {
		m |= kmWin
	}
	return m
}

func isWindowFullScreen(w uintptr) bool {
	wr, ok := getWindowRect(w)
	if !ok {
		return false
	}
	mi, ok := getMonitorInfo(monitorFromWindow(w))
	if !ok {
		return false
	}
	const T = 2
	r := mi.RcMonitor
	return wr.Left <= r.Left+T && wr.Top <= r.Top+T &&
		wr.Right >= r.Right-T && wr.Bottom >= r.Bottom-T
}

// Перехватываем только когда полноэкранное RAIL_WINDOW RDP-клиента в фокусе.
func isRemoteFocused() bool {
	fg := getForegroundWindow()
	if fg == 0 {
		return false
	}
	pid := getWindowThreadProcessId(fg)
	if pid == 0 || pid == getCurrentProcessId() {
		return false
	}
	if !classNameEqual(fg, "RAIL_WINDOW") {
		return false
	}
	if !isWindowFullScreen(fg) {
		return false
	}
	if findIHWindow(pid) == 0 {
		return false
	}
	targetPid = pid
	return true
}

// Win+Z: сворачиваем локальное полноэкранное окно RAIL (уже подтверждено
// isRemoteFocused, что это оно в фокусе). Async — не блокируем hook-поток.
func minimizeRemote() {
	if fg := getForegroundWindow(); fg != 0 {
		procShowWindowAsync.Call(fg, SW_MINIMIZE)
	}
}

func sendWinTap() {
	postSeq([]Scan{
		{winScan, false, winExt, winWireVK, false},
		{winScan, true, winExt, winWireVK, false},
	})
}

func sendWinChord(scan int, ext bool, vk int) {
	postSeq([]Scan{
		{winScan, false, winExt, winWireVK, false},
		{scan, false, ext, vk, false},
		{scan, true, ext, vk, false},
		{winScan, true, winExt, winWireVK, false},
	})
}

// Режим "tab" исходника: Alt в сеанс не шлём (его пробрасывает сам контрол),
// Tab уходит с noSys как WM_KEYDOWN. Shift для Alt+Shift+Tab — sticky.
func sendAltTab(shift bool, tabScan int, tabExt bool) {
	var s []Scan
	if shift && !shiftSticky {
		s = append(s, Scan{shiftScan, false, shiftExt, VK_SHIFT, false})
		shiftSticky = true
	}
	s = append(s, Scan{tabScan, false, tabExt, VK_TAB, true})
	s = append(s, Scan{tabScan, true, tabExt, VK_TAB, true})
	postSeq(s)
}

func releaseSticky() {
	if !altSticky && !shiftSticky {
		return
	}
	var s []Scan
	if shiftSticky {
		s = append(s, Scan{shiftScan, true, shiftExt, VK_SHIFT, false})
		shiftSticky = false
	}
	if altSticky {
		s = append(s, Scan{altScan, true, altExt, VK_MENU, false})
		altSticky = false
	}
	postSeq(s)
}

// Под-флаг Win+<клавиша> по vk. Неизвестная клавиша -> winOther.
func winComboAllowed(vk int) bool {
	switch vk {
	case 0x52:
		return gHotkeys.WinR
	case 0x45:
		return gHotkeys.WinE
	case 0x44:
		return gHotkeys.WinD
	case 0x09:
		return gHotkeys.WinTab
	case 0x56:
		return gHotkeys.WinV
	}
	return gHotkeys.WinOther
}

func handle(wParam uintptr, kb *KBDLLHOOKSTRUCT) bool {
	if kb.Flags&LLKHF_INJECTED != 0 {
		return false
	}
	down := wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN
	up := wParam == WM_KEYUP || wParam == WM_SYSKEYUP
	if !down && !up {
		return false
	}
	vk := int(kb.VkCode)

	// Состояние Win ведём по событиям хука, а не по GetAsyncKeyState: Win-down
	// мы глушим сами (return true), а заглушённая клавиша асинхронное состояние
	// не обновляет — IsDown(VK_LWIN) остаётся ложью на всё удержание.
	isWinKey := vk == VK_LWIN || vk == VK_RWIN
	if winDown && getTickCount64()-winDownTick > winHoldMaxMS {
		winDown = false
		winCombo = false
	}

	// Пока Win зажат — любая клавиша идёт в аккорд; иначе только клавиши из списка.
	if !winDown && !isWatchedKey(vk) {
		return false
	}

	sc := int(kb.ScanCode)
	ext := kb.Flags&LLKHF_EXTENDED != 0

	if !isRemoteFocused() {
		releaseSticky()
		winDown = false
		return false
	}

	if vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU {
		if down {
			altScan = sc
			altExt = ext
		} else if altSticky {
			postSeq([]Scan{{altScan, true, altExt, VK_MENU, false}})
			altSticky = false
		}
		return false
	}
	if vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT {
		if down {
			shiftScan = sc
			shiftExt = ext
		} else if shiftSticky {
			postSeq([]Scan{{shiftScan, true, shiftExt, VK_SHIFT, false}})
			shiftSticky = false
		}
		return false
	}
	if gHotkeys.Win && isWinKey {
		if down {
			if !winDown {
				winDown = true
				winCombo = false
				winScan = sc
				winExt = ext
				winDownTick = getTickCount64()
			}
		} else {
			if winDown && !winCombo {
				sendWinTap()
			}
			winDown = false
		}
		return true
	}
	if winDown && gHotkeys.Win {
		if isModifierKey(vk) {
			return false
		}
		if down {
			winCombo = true
			if gHotkeys.WinZ && vk == 0x5A { // Win+Z — свернуть удалёнку локально, не форвардим
				minimizeRemote()
			} else if winComboAllowed(vk) {
				// Выключенный под-флаг Win+<кл> при включённом мастере: глушим (no-op).
				sendWinChord(sc, ext, vk)
			}
		}
		return true
	}
	if vk == VK_TAB {
		m := modMask()
		if m == kmAlt {
			if !gHotkeys.AltTab {
				return false
			}
			if down {
				sendAltTab(false, sc, ext)
			}
			return true
		}
		if m == (kmAlt | kmShift) {
			if !gHotkeys.AltShiftTab {
				return false
			}
			if down {
				sendAltTab(true, sc, ext)
			}
			return true
		}
		return false
	}
	if gHotkeys.CtrlEsc && vk == VK_ESCAPE {
		if modMask() != kmCtrl {
			return false
		}
		if down {
			e := sc
			if e == 0 {
				e = scEsc
			}
			postSeq([]Scan{
				{scLCtrl, false, false, VK_CONTROL, false},
				{e, false, ext, VK_ESCAPE, false},
				{e, true, ext, VK_ESCAPE, false},
				{scLCtrl, true, false, VK_CONTROL, false},
			})
		}
		return true
	}
	return false
}

func hookProc(nCode uintptr, wParam uintptr, lParam uintptr) uintptr {
	if int32(nCode) == HC_ACTION {
		kb := (*KBDLLHOOKSTRUCT)(unsafe.Pointer(lParam))
		if handle(wParam, kb) {
			return 1
		}
	}
	r, _, _ := procCallNextHookEx.Call(hookHandle, nCode, wParam, lParam)
	return r
}
