// Глобальный low-level хук клавиатуры + проброс сочетаний в IHWindowClass-окно
// полноэкранного RemoteApp-сеанса межпроцессным PostMessageW (без инъекции).
#include "rdpkey.h"

#define WIN_WIRE_VK   0x41 // Win шлём с vk 0x41 (scan 0x5B) — обход фильтра mstscax
#define WIN_HOLD_MAX  30000
#define SC_LSHIFT     0x2A
#define SC_LCTRL      0x1D
#define SC_ESC        0x01

Hotkeys gHotkeys;
HHOOK   g_hook;
HWND    g_targetWnd;
HWND    g_boundRail;

typedef struct { int scan; BOOL up; BOOL ext; int vk; BOOL noSys; } Scan;

static int       winScan = 0x5B;
static BOOL      winExt = TRUE;
static BOOL      winDown = FALSE, winCombo = FALSE;
static ULONGLONG winDownTick = 0;
static BOOL      shiftSticky = FALSE;
static int       shiftScan = SC_LSHIFT;
static BOOL      shiftExt = FALSE;
static HWND      ihCacheRail = NULL, ihCacheWnd = NULL;

static LPARAM makeLParam(int scan, BOOL ext, BOOL up, BOOL altCtx) {
    LPARAM l = 1; // repeat count = 1
    l |= (LPARAM)(scan & 0xFF) << 16;
    if (ext)    l |= (LPARAM)1 << 24;
    if (altCtx) l |= (LPARAM)1 << 29;
    if (up)     l |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    return l;
}

// ---- поиск IHWindowClass ----

static HWND  s_findWnd;
static DWORD s_findPid;

static BOOL CALLBACK enumChildIH(HWND h, LPARAM lp) {
    (void)lp;
    if (class_name_equal(h, L"IHWindowClass")) { s_findWnd = h; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK enumTopIH(HWND top, LPARAM lp) {
    (void)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);
    if (pid == s_findPid) {
        if (class_name_equal(top, L"IHWindowClass")) { s_findWnd = top; return FALSE; }
        EnumChildWindows(top, enumChildIH, 0);
        if (s_findWnd) return FALSE;
    }
    return TRUE;
}
static HWND resolveIH(HWND fg, DWORD pid) {
    s_findWnd = NULL;
    EnumChildWindows(fg, enumChildIH, 0);
    if (s_findWnd) return s_findWnd;
    s_findPid = pid;
    s_findWnd = NULL;
    EnumWindows(enumTopIH, 0);
    return s_findWnd;
}
static HWND findIHForRail(HWND fg, DWORD pid) {
    if (!fg) return NULL;
    if (ihCacheRail == fg && ihCacheWnd && IsWindow(ihCacheWnd)) return ihCacheWnd;
    HWND ih = resolveIH(fg, pid);
    ihCacheRail = fg;
    ihCacheWnd = ih;
    return ih;
}

BOOL is_remote_focused(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return FALSE;
    if (!class_name_equal(fg, L"RAIL_WINDOW")) return FALSE;
    if (!window_fullscreen(fg)) return FALSE;
    HWND ih = findIHForRail(fg, pid);
    if (!ih) return FALSE;
    g_targetWnd = ih;
    if (is_our_rail(fg)) g_boundRail = fg;
    return TRUE;
}

static void postSeq(const Scan *seq, int n) {
    HWND t = g_targetWnd;
    if (!t || n <= 0) return;
    BOOL altDown = key_down(VK_MENU);
    for (int i = 0; i < n; i++) {
        Scan s = seq[i];
        BOOL ctx = FALSE;
        if (!s.noSys) ctx = altDown || s.vk == VK_MENU;
        UINT msg;
        if (s.up) msg = ctx ? WM_SYSKEYUP : WM_KEYUP;
        else      msg = ctx ? WM_SYSKEYDOWN : WM_KEYDOWN;
        PostMessageW(t, msg, (WPARAM)s.vk, makeLParam(s.scan, s.ext, s.up, ctx));
    }
}

static BOOL isWatchedKey(int vk) {
    switch (vk) {
        case VK_LWIN: case VK_RWIN:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_TAB: case VK_ESCAPE: case VK_SNAPSHOT:
            return TRUE;
    }
    return FALSE;
}
static BOOL isModifierKey(int vk) {
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_LWIN: case VK_RWIN:
            return TRUE;
    }
    return FALSE;
}
static int modMask(void) {
    int m = 0;
    if (key_down(VK_MENU)) m |= KM_ALT;
    if (key_down(VK_CONTROL)) m |= KM_CTRL;
    if (key_down(VK_SHIFT)) m |= KM_SHIFT;
    if (key_down(VK_LWIN) || key_down(VK_RWIN)) m |= KM_WIN;
    return m;
}

static void minimizeRemote(void) {
    HWND fg = GetForegroundWindow();
    if (fg) ShowWindowAsync(fg, SW_MINIMIZE);
}

static void sendWinTap(void) {
    Scan s[2] = {
        {winScan, FALSE, winExt, WIN_WIRE_VK, FALSE},
        {winScan, TRUE,  winExt, WIN_WIRE_VK, FALSE},
    };
    postSeq(s, 2);
}
static void sendWinChord(int scan, BOOL ext, int vk) {
    Scan s[4] = {
        {winScan, FALSE, winExt, WIN_WIRE_VK, FALSE},
        {scan,    FALSE, ext,    vk,          FALSE},
        {scan,    TRUE,  ext,    vk,          FALSE},
        {winScan, TRUE,  winExt, WIN_WIRE_VK, FALSE},
    };
    postSeq(s, 4);
}
static void sendWinShiftChord(int scan, BOOL ext, int vk) {
    Scan s[6] = {
        {winScan,   FALSE, winExt,   WIN_WIRE_VK, FALSE},
        {shiftScan, FALSE, shiftExt, VK_SHIFT,    FALSE},
        {scan,      FALSE, ext,      vk,          FALSE},
        {scan,      TRUE,  ext,      vk,          FALSE},
        {shiftScan, TRUE,  shiftExt, VK_SHIFT,    FALSE},
        {winScan,   TRUE,  winExt,   WIN_WIRE_VK, FALSE},
    };
    postSeq(s, 6);
}
static void sendAltTab(BOOL shift, int tabScan, BOOL tabExt) {
    Scan s[3];
    int n = 0;
    if (shift && !shiftSticky) {
        s[n].scan = shiftScan; s[n].up = FALSE; s[n].ext = shiftExt; s[n].vk = VK_SHIFT; s[n].noSys = FALSE; n++;
        shiftSticky = TRUE;
    }
    s[n].scan = tabScan; s[n].up = FALSE; s[n].ext = tabExt; s[n].vk = VK_TAB; s[n].noSys = TRUE; n++;
    s[n].scan = tabScan; s[n].up = TRUE;  s[n].ext = tabExt; s[n].vk = VK_TAB; s[n].noSys = TRUE; n++;
    postSeq(s, n);
}

void release_sticky(void) {
    if (!shiftSticky) return;
    if (g_targetWnd && IsWindow(g_targetWnd)) {
        Scan s[1] = {{shiftScan, TRUE, shiftExt, VK_SHIFT, FALSE}};
        postSeq(s, 1);
    }
    shiftSticky = FALSE;
}

void reset_win_state(void) {
    winDown = FALSE;
    winCombo = FALSE;
}

static BOOL winComboAllowed(int vk) {
    switch (vk) {
        case 0x52: return gHotkeys.winR;
        case 0x45: return gHotkeys.winE;
        case 0x44: return gHotkeys.winD;
        case 0x09: return gHotkeys.winTab;
        case 0x56: return gHotkeys.winV;
    }
    return gHotkeys.winOther;
}

static BOOL handle(WPARAM wParam, const KBDLLHOOKSTRUCT *kb) {
    if (kb->flags & LLKHF_INJECTED) return FALSE;
    BOOL down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    BOOL up   = wParam == WM_KEYUP   || wParam == WM_SYSKEYUP;
    if (!down && !up) return FALSE;
    int vk = (int)kb->vkCode;

    BOOL isWinKey = vk == VK_LWIN || vk == VK_RWIN;
    if (winDown && GetTickCount64() - winDownTick > WIN_HOLD_MAX) {
        winDown = FALSE;
        winCombo = FALSE;
    }

    if (!winDown && !isWatchedKey(vk)) return FALSE;

    int sc = (int)kb->scanCode;
    BOOL ext = (kb->flags & LLKHF_EXTENDED) != 0;

    if (!is_remote_focused()) {
        release_sticky();
        winDown = FALSE;
        return FALSE;
    }

    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return FALSE;

    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        if (down) { shiftScan = sc; shiftExt = ext; }
        else if (shiftSticky) {
            Scan s[1] = {{shiftScan, TRUE, shiftExt, VK_SHIFT, FALSE}};
            postSeq(s, 1);
            shiftSticky = FALSE;
        }
        return FALSE;
    }

    if (gHotkeys.win && isWinKey) {
        if (down) {
            if (!winDown) {
                winDown = TRUE; winCombo = FALSE;
                winScan = sc; winExt = ext;
                winDownTick = GetTickCount64();
            }
        } else {
            if (winDown && !winCombo) sendWinTap();
            winDown = FALSE;
        }
        return TRUE;
    }

    if (winDown && gHotkeys.win) {
        if (isModifierKey(vk)) return FALSE;
        if (down) {
            winCombo = TRUE;
            if (gHotkeys.winZ && vk == 0x5A) {
                minimizeRemote();
            } else if (vk == 0x53 && key_down(VK_SHIFT)) {
                if (gHotkeys.winShiftS) sendWinShiftChord(sc, ext, vk);
            } else if (winComboAllowed(vk)) {
                sendWinChord(sc, ext, vk);
            }
        }
        return TRUE;
    }

    if (vk == VK_SNAPSHOT) {
        if (!gHotkeys.printScreen) return FALSE;
        if (down) {
            Scan s[2] = {
                {sc, FALSE, ext, VK_SNAPSHOT, FALSE},
                {sc, TRUE,  ext, VK_SNAPSHOT, FALSE},
            };
            postSeq(s, 2);
        }
        return TRUE;
    }

    if (vk == VK_TAB) {
        int m = modMask();
        if (m == KM_ALT) {
            if (!gHotkeys.altTab) return FALSE;
            if (down) sendAltTab(FALSE, sc, ext);
            return TRUE;
        }
        if (m == (KM_ALT | KM_SHIFT)) {
            if (!gHotkeys.altShiftTab) return FALSE;
            if (down) sendAltTab(TRUE, sc, ext);
            return TRUE;
        }
        return FALSE;
    }

    if (gHotkeys.ctrlEsc && vk == VK_ESCAPE) {
        if (modMask() != KM_CTRL) return FALSE;
        if (down) {
            int e = sc ? sc : SC_ESC;
            Scan s[4] = {
                {SC_LCTRL, FALSE, FALSE, VK_CONTROL, FALSE},
                {e,        FALSE, ext,   VK_ESCAPE,  FALSE},
                {e,        TRUE,  ext,   VK_ESCAPE,  FALSE},
                {SC_LCTRL, TRUE,  FALSE, VK_CONTROL, FALSE},
            };
            postSeq(s, 4);
        }
        return TRUE;
    }

    return FALSE;
}

LRESULT CALLBACK hook_proc(int nCode, WPARAM w, LPARAM l) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT *kb = (const KBDLLHOOKSTRUCT *)l;
        if (handle(w, kb)) return 1;
    }
    return CallNextHookEx(g_hook, nCode, w, l);
}

BOOL install_hook(void) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, hook_proc, g_hInst, 0);
    return g_hook != NULL;
}

void remove_hook(void) {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
}
