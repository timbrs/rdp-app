#include <windows.h>
#include <stdio.h>
#include <conio.h>

static const bool g_fwdWin = true, g_fwdWinCombo = true, g_fwdAltTab = true, g_fwdCtrlEsc = true;
static const char g_altMode[16] = "tab";

// noSys: слать KEYDOWN даже при зажатом Alt (обход фильтра Alt+Tab в mstscax).
struct Scan { int scan; bool up; bool ext; int vk; bool noSys; };

static LPARAM MakeLParam(int scan, bool ext, bool up, bool altCtx) {
    LPARAM l = 1;
    l |= (LPARAM)(scan & 0xFF) << 16;
    if (ext)    l |= (LPARAM)1 << 24;
    if (altCtx) l |= (LPARAM)1 << 29;
    if (up)     l |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    return l;
}

static DWORD g_targetPid = 0;
static DWORD g_findPid = 0;
static HWND  g_findWnd = NULL;
static BOOL CALLBACK EnumChildIH(HWND h, LPARAM) {
    char cls[64]; GetClassNameA(h, cls, sizeof(cls));
    if (lstrcmpiA(cls, "IHWindowClass") == 0) { g_findWnd = h; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK EnumTopIH(HWND top, LPARAM) {
    DWORD p = 0; GetWindowThreadProcessId(top, &p);
    if (p == g_findPid) {
        char cls[64]; GetClassNameA(top, cls, sizeof(cls));
        if (lstrcmpiA(cls, "IHWindowClass") == 0) { g_findWnd = top; return FALSE; }
        EnumChildWindows(top, EnumChildIH, 0);
        if (g_findWnd) return FALSE;
    }
    return TRUE;
}
static DWORD g_ihCachePid = 0;
static HWND  g_ihCacheWnd = NULL;
static HWND FindIHWindow(DWORD pid) {
    if (!pid) return NULL;
    if (g_ihCachePid == pid && g_ihCacheWnd && IsWindow(g_ihCacheWnd)) return g_ihCacheWnd;
    g_findPid = pid; g_findWnd = NULL;
    EnumWindows(EnumTopIH, 0);
    g_ihCachePid = pid; g_ihCacheWnd = g_findWnd;
    return g_findWnd;
}

static void PostSeq(const Scan* seq, int n) {
    HWND t = FindIHWindow(g_targetPid);
    if (!t || n <= 0) return;
    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    for (int i = 0; i < n; i++) {
        bool ctx = seq[i].noSys ? false : (altDown || seq[i].vk == VK_MENU);
        UINT msg = seq[i].up ? (ctx ? WM_SYSKEYUP : WM_KEYUP)
                             : (ctx ? WM_SYSKEYDOWN : WM_KEYDOWN);
        PostMessageW(t, msg, (WPARAM)seq[i].vk, MakeLParam(seq[i].scan, seq[i].ext, seq[i].up, ctx));
    }
}
static void Enqueue(const Scan* seq, int n) { PostSeq(seq, n); }

static const int SC_LALT = 0x38, SC_LSHIFT = 0x2A, SC_LCTRL = 0x1D, SC_ESC = 0x01;
static const int WIN_WIRE_VK = 0x41;  // Win: scan 0x5B, но vk 0x41 — обход фильтра Win-клавиши

static bool s_winDown = false, s_winCombo = false; static int s_winScan = 0x5B; static bool s_winExt = true;
static bool s_altSticky = false;  static int s_altScan = SC_LALT;  static bool s_altExt = false;
static bool s_shiftSticky = false; static int s_shiftScan = SC_LSHIFT; static bool s_shiftExt = false;
static HHOOK s_hook = NULL;

static bool IsDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Клавиши, участвующие в пробрасываемых сочетаниях. Всё остальное хук
// не рассматривает: выходим до чтения scan-кода и до проверки фокуса.
static bool IsWatchedKey(int vk) {
    switch (vk) {
    case VK_LWIN:  case VK_RWIN:
    case VK_MENU:  case VK_LMENU:  case VK_RMENU:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_TAB:   case VK_ESCAPE:
        return true;
    }
    return false;
}

static bool IsModifierKey(int vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        return true;
    }
    return IsWatchedKey(vk) && vk != VK_TAB && vk != VK_ESCAPE;
}

// Префикс KM_, а не MOD_: MOD_ALT/MOD_SHIFT/MOD_WIN заняты макросами winuser.h.
enum { KM_ALT = 1, KM_CTRL = 2, KM_SHIFT = 4, KM_WIN = 8 };

static int ModMask() {
    int m = 0;
    if (IsDown(VK_MENU))    m |= KM_ALT;
    if (IsDown(VK_CONTROL)) m |= KM_CTRL;
    if (IsDown(VK_SHIFT))   m |= KM_SHIFT;
    if (IsDown(VK_LWIN) || IsDown(VK_RWIN)) m |= KM_WIN;
    return m;
}

static bool IsWindowFullScreen(HWND w) {
    RECT wr;
    if (!GetWindowRect(w, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const int T = 2;
    return wr.left <= mi.rcMonitor.left + T && wr.top <= mi.rcMonitor.top + T &&
           wr.right >= mi.rcMonitor.right - T && wr.bottom >= mi.rcMonitor.bottom - T;
}

// Перехватываем только когда полноэкранное RAIL_WINDOW RDP-клиента в фокусе.
static bool IsRemoteFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    char cls[64]; GetClassNameA(fg, cls, sizeof(cls));
    if (lstrcmpiA(cls, "RAIL_WINDOW") != 0) return false;
    if (!IsWindowFullScreen(fg)) return false;
    if (!FindIHWindow(pid)) return false;
    g_targetPid = pid;
    return true;
}

static void SendWinTap() {
    Scan s[2] = { {s_winScan,false,s_winExt,WIN_WIRE_VK}, {s_winScan,true,s_winExt,WIN_WIRE_VK} };
    Enqueue(s, 2);
}
static void SendWinChord(int scan, bool ext, int vk) {
    Scan s[4] = { {s_winScan,false,s_winExt,WIN_WIRE_VK}, {scan,false,ext,vk}, {scan,true,ext,vk}, {s_winScan,true,s_winExt,WIN_WIRE_VK} };
    Enqueue(s, 4);
}
static void SendAltTab(bool shift, int tabScan, bool tabExt) {
    Scan s[6]; int n = 0;
    if (lstrcmpiA(g_altMode, "chord") == 0) {
        s[n++] = {s_altScan,false,s_altExt,VK_MENU};
        if (shift) s[n++] = {s_shiftScan,false,s_shiftExt,VK_SHIFT};
        s[n++] = {tabScan,false,tabExt,VK_TAB,true};
        s[n++] = {tabScan,true,tabExt,VK_TAB,true};
        if (shift) s[n++] = {s_shiftScan,true,s_shiftExt,VK_SHIFT};
        s[n++] = {s_altScan,true,s_altExt,VK_MENU};
    } else {
        if (lstrcmpiA(g_altMode, "tab") != 0 && !s_altSticky) { s[n++] = {s_altScan,false,s_altExt,VK_MENU}; s_altSticky = true; }
        if (shift && !s_shiftSticky) { s[n++] = {s_shiftScan,false,s_shiftExt,VK_SHIFT}; s_shiftSticky = true; }
        s[n++] = {tabScan,false,tabExt,VK_TAB,true};
        s[n++] = {tabScan,true,tabExt,VK_TAB,true};
    }
    Enqueue(s, n);
}
static void ReleaseSticky() {
    if (!s_altSticky && !s_shiftSticky) return;
    Scan s[2]; int n = 0;
    if (s_shiftSticky) { s[n++] = {s_shiftScan,true,s_shiftExt,VK_SHIFT}; s_shiftSticky = false; }
    if (s_altSticky)   { s[n++] = {s_altScan,true,s_altExt,VK_MENU};     s_altSticky = false; }
    Enqueue(s, n);
}

static bool Handle(WPARAM wParam, LPARAM lParam) {
    KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
    if (kb->flags & LLKHF_INJECTED) return false;
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!down && !up) return false;
    int vk = kb->vkCode;

    // Win мог быть отпущен, пока фокус был вне сессии — сверяемся с железом,
    // иначе залипший s_winDown глотал бы посторонние клавиши. События самой
    // Win-клавиши исключены: на её up железо уже показывает «отпущена»
    // (async-состояние обновляется до вызова хука), и сброс здесь съел бы тап.
    bool isWinKey = (vk == VK_LWIN || vk == VK_RWIN);
    if (s_winDown && !isWinKey && !IsDown(VK_LWIN) && !IsDown(VK_RWIN)) { s_winDown = false; s_winCombo = false; }

    // Пока Win зажат — любая клавиша идёт в аккорд; иначе только клавиши из списка.
    if (!s_winDown && !IsWatchedKey(vk)) return false;

    int sc = kb->scanCode;
    bool ext = (kb->flags & LLKHF_EXTENDED) != 0;

    if (!IsRemoteFocused()) { ReleaseSticky(); s_winDown = false; return false; }

    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        if (down) { s_altScan = sc; s_altExt = ext; }
        else if (s_altSticky) { Scan s = {s_altScan,true,s_altExt,VK_MENU}; Enqueue(&s,1); s_altSticky = false; }
        return false;
    }
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        if (down) { s_shiftScan = sc; s_shiftExt = ext; }
        else if (s_shiftSticky) { Scan s = {s_shiftScan,true,s_shiftExt,VK_SHIFT}; Enqueue(&s,1); s_shiftSticky = false; }
        return false;
    }
    if (g_fwdWin && isWinKey) {
        if (down) { if (!s_winDown) { s_winDown = true; s_winCombo = false; s_winScan = sc; s_winExt = ext; } }
        else { if (s_winDown && !s_winCombo) SendWinTap(); s_winDown = false; }
        return true;
    }
    if (s_winDown && g_fwdWinCombo) {
        if (IsModifierKey(vk)) return false;
        if (down) { s_winCombo = true; SendWinChord(sc, ext, vk); }
        return true;
    }
    if (g_fwdAltTab && vk == VK_TAB) {
        int m = ModMask();
        if (m != KM_ALT && m != (KM_ALT | KM_SHIFT)) return false;
        if (down) SendAltTab((m & KM_SHIFT) != 0, sc, ext);
        return true;
    }
    if (g_fwdCtrlEsc && vk == VK_ESCAPE) {
        if (ModMask() != KM_CTRL) return false;
        if (down) {
            int e = sc ? sc : SC_ESC;
            Scan s[4] = { {SC_LCTRL,false,false,VK_CONTROL}, {e,false,ext,VK_ESCAPE}, {e,true,ext,VK_ESCAPE}, {SC_LCTRL,true,false,VK_CONTROL} };
            Enqueue(s, 4);
        }
        return true;
    }
    return false;
}

static LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && Handle(wParam, lParam)) return 1;
    return CallNextHookEx(s_hook, nCode, wParam, lParam);
}

// Живая (в т.ч. свёрнутая) сессия = видимое RAIL_WINDOW. Процессы mstsc считать
// нельзя: фоновый mstsc.exe -Embedding переживает закрытие и держит скрытые окна.
static int g_visRail;
static BOOL CALLBACK CountRailProc(HWND h, LPARAM) {
    if (!IsWindowVisible(h)) return TRUE;
    char cls[64]; GetClassNameA(h, cls, sizeof(cls));
    if (lstrcmpiA(cls, "RAIL_WINDOW") == 0) g_visRail++;
    return TRUE;
}
static int VisibleRailCount() { g_visRail = 0; EnumWindows(CountRailProc, 0); return g_visRail; }

static int RunMode(LPCWSTR rdp) {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    wchar_t mstsc[MAX_PATH]; wsprintfW(mstsc, L"%s\\mstsc.exe", sys);
    wchar_t cmd[2048];
    if (rdp && *rdp) wsprintfW(cmd, L"\"%s\" \"%s\"", mstsc, rdp);
    else             wsprintfW(cmd, L"\"%s\"", mstsc);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(mstsc, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxW(NULL, L"Не удалось запустить mstsc.exe.", L"rdpkey", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(NULL), 0);
    if (!s_hook) {
        MessageBoxW(NULL, L"Не удалось установить LL-хук.", L"rdpkey", MB_ICONERROR);
        return 1;
    }

    SetTimer(NULL, 1, 2000, NULL);
    ULONGLONG start = GetTickCount64();
    bool seen = false; int zero = 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            int c = VisibleRailCount();
            if (c > 0) { seen = true; zero = 0; }
            else if (seen) { if (++zero >= 2) PostQuitMessage(0); }
            else if (GetTickCount64() - start > 120000) PostQuitMessage(0);
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    UnhookWindowsHookEx(s_hook);
    return 0;
}

static void HelpMode() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONIN$",  "r", stdin);
    SetConsoleOutputCP(CP_UTF8);

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    char exeU[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeU, sizeof(exeU), NULL, NULL);

    printf("\nrdpkey — проброс горячих клавиш в RDP RemoteApp\n\n");
    printf("Пробрасывает Win, Win+клавишу, Alt+Tab, Alt+Shift+Tab, Ctrl+Esc\n");
    printf("в полноэкранный RemoteApp-сеанс. В реестр себя не прописывает.\n\n");
    printf("Ассоциация с .rdp — вручную: правый клик по .rdp ->\n");
    printf("«Открыть с помощью» -> «Выбрать другое приложение» -> указать:\n");
    printf("  %s\n\n", exeU);
    printf("Нажмите любую клавишу для выхода...\n");
    _getch();
}

static bool ArgAfterProgram(LPWSTR cl, wchar_t* out, int cap) {
    LPWSTR p = cl;
    while (*p == L' ' || *p == L'\t') p++;
    if (*p == L'"') { p++; while (*p && *p != L'"') p++; if (*p) p++; }
    else            { while (*p && *p != L' ' && *p != L'\t') p++; }
    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) return false;
    int i = 0;
    if (*p == L'"') { p++; while (*p && *p != L'"' && i < cap - 1) out[i++] = *p++; }
    else            { while (*p && i < cap - 1) out[i++] = *p++; }
    out[i] = 0;
    return i > 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    wchar_t arg[MAX_PATH * 2];
    if (ArgAfterProgram(GetCommandLineW(), arg, MAX_PATH * 2))
        return RunMode(arg);
    HelpMode();
    return 0;
}
