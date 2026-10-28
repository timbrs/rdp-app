// Отладочный лог: %LOCALAPPDATA%\rdpkey\logs\rdpkey-ГГГГ-ММ-ДД.log, только дописывается.
#ifdef RDPKEY_LOG
#include "rdpkey.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

ULONGLONG g_hookTick;

static HANDLE s_log = INVALID_HANDLE_VALUE;
static WORD   s_day;

static void open_log(const SYSTEMTIME *st) {
    if (s_log != INVALID_HANDLE_VALUE) CloseHandle(s_log);
    wchar_t base[1024], dir[1100], path[1200];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, 1024) == 0) base[0] = 0;
    swprintf(dir, 1100, L"%ls\\rdpkey", base);
    CreateDirectoryW(dir, NULL);
    swprintf(dir, 1100, L"%ls\\rdpkey\\logs", base);
    CreateDirectoryW(dir, NULL);
    swprintf(path, 1200, L"%ls\\rdpkey-%04u-%02u-%02u.log", dir, st->wYear, st->wMonth, st->wDay);
    s_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    s_day = st->wDay;
}

void dlog(const wchar_t *fmt, ...) {
    DWORD err = GetLastError();
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (s_log == INVALID_HANDLE_VALUE || st.wDay != s_day) open_log(&st);
    if (s_log != INVALID_HANDLE_VALUE) {
        wchar_t line[4096];
        int n = swprintf(line, 64, L"%02u:%02u:%02u.%03u [%lu] ", st.wHour, st.wMinute,
                         st.wSecond, st.wMilliseconds, GetCurrentProcessId());
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf(line + n, 4096 - n - 3, fmt, ap);
        va_end(ap);
        line[4096 - 3] = 0;
        wcscat(line, L"\r\n");
        char u8[4096 * 3];
        int b = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL);
        DWORD wr;
        if (b > 1) WriteFile(s_log, u8, (DWORD)(b - 1), &wr, NULL);
    }
    SetLastError(err);
}

static void describe(HWND h, wchar_t *out, int cch) {
    wchar_t cls[64] = L"", title[160] = L"";
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, title, 160);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT r = {0, 0, 0, 0};
    GetWindowRect(h, &r);
    swprintf(out, cch, L"hwnd=%p pid=%lu cls=%ls vis=%d min=%d fs=%d rect=(%ld,%ld,%ld,%ld) "
             L"st=%08lX ex=%08lX own=%p \"%ls\"",
             (void *)h, pid, cls, IsWindowVisible(h) ? 1 : 0, IsIconic(h) ? 1 : 0,
             window_fullscreen(h) ? 1 : 0, r.left, r.top, r.right, r.bottom,
             (unsigned long)GetWindowLongPtrW(h, GWL_STYLE), (unsigned long)GetWindowLongPtrW(h, GWL_EXSTYLE),
             (void *)GetWindow(h, GW_OWNER), title);
}

void dlog_wnd(const wchar_t *what, HWND h) {
    if (!h) { dlog(L"%ls: (нет)", what); return; }
    wchar_t d[512];
    describe(h, d, 512);
    dlog(L"%ls: %ls", what, d);
}

#define SNAP_CCH 32768
static wchar_t s_cur[SNAP_CCH], s_prev[SNAP_CCH];
static int     s_curLen;
static DWORD   s_pids[16];
static int     s_npid;

static void add_pid(DWORD p) {
    for (int i = 0; i < s_npid; i++) if (s_pids[i] == p) return;
    if (s_npid < 16) s_pids[s_npid++] = p;
}
static BOOL has_pid(DWORD p) {
    for (int i = 0; i < s_npid; i++) if (s_pids[i] == p) return TRUE;
    return FALSE;
}

static BOOL CALLBACK enumRailPids(HWND h, LPARAM lp) {
    (void)lp;
    if (class_name_equal(h, L"RAIL_WINDOW")) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        add_pid(pid);
    }
    return TRUE;
}

static BOOL CALLBACK enumSnap(HWND h, LPARAM lp) {
    (void)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    BOOL rail = class_name_equal(h, L"RAIL_WINDOW");
    if (!rail && !(has_pid(pid) && IsWindowVisible(h))) return TRUE;
    wchar_t d[512];
    describe(h, d, 512);
    const wchar_t *tag = rail ? (is_our_rail(h) ? L"наш " : L"база") : L"окно";
    int n = _snwprintf(s_cur + s_curLen, SNAP_CCH - s_curLen - 1, L"  %ls %ls\n", tag, d);
    if (n > 0) s_curLen += n;
    s_cur[SNAP_CCH - 1] = 0;
    return TRUE;
}

void dlog_windows(DWORD launchedPid) {
    s_npid = 0;
    if (launchedPid) add_pid(launchedPid);
    EnumWindows(enumRailPids, 0);
    s_curLen = 0;
    s_cur[0] = 0;
    EnumWindows(enumSnap, 0);
    if (wcscmp(s_cur, s_prev) == 0) return;
    wcscpy(s_prev, s_cur);
    dlog(L"окна RDP изменились (процессы с RAIL + запущенный mstsc):");
    if (!s_cur[0]) { dlog(L"  (окон нет)"); return; }
    wchar_t *ctx = NULL;
    for (wchar_t *l = wcstok(s_cur, L"\n", &ctx); l; l = wcstok(NULL, L"\n", &ctx))
        dlog(L"%ls", l);
}

static void module_of(void *addr, wchar_t *name, int cch, ULONG_PTR *off) {
    HMODULE mod = NULL;
    name[0] = 0;
    *off = (ULONG_PTR)addr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod) && mod) {
        GetModuleFileNameW(mod, name, cch);
        *off = (ULONG_PTR)addr - (ULONG_PTR)mod;
    }
}

static LONG s_vehCount;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code & 0xF0000000) == 0xC0000000 && InterlockedIncrement(&s_vehCount) <= 50) {
        wchar_t name[MAX_PATH];
        ULONG_PTR off;
        module_of(ep->ExceptionRecord->ExceptionAddress, name, MAX_PATH, &off);
        dlog(L"ИСКЛЮЧЕНИЕ (first-chance) code=0x%08lX в %ls+0x%Ix поток %lu", code, name, off,
             GetCurrentThreadId());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    wchar_t name[MAX_PATH];
    ULONG_PTR off;
    module_of(ep->ExceptionRecord->ExceptionAddress, name, MAX_PATH, &off);
    dlog(L"ПАДЕНИЕ code=0x%08lX в %ls+0x%Ix поток %lu", ep->ExceptionRecord->ExceptionCode, name, off,
         GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}

static void log_exit(void) {
    dlog(L"процесс завершается (exit)");
}

void dlog_init(void) {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    dlog(L"==== rdpkey %ls LOG-сборка, exe=%ls, сеанс Windows %lu", APP_VERSION, exe, sid);
    dlog(L"командная строка: %ls", GetCommandLineW());
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(crash_filter);
    atexit(log_exit);
}
#endif
