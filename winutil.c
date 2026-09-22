// Разделяемые тонкие обёртки над Win32 (используются hook/session/gui/errdialog).
#include "rdpkey.h"
#include <shellapi.h>
#include <wchar.h>
#include <stdio.h>

BOOL class_name_equal(HWND h, const wchar_t *want) {
    wchar_t buf[64];
    int n = GetClassNameW(h, buf, 64);
    if (n <= 0) return FALSE;
    return _wcsicmp(buf, want) == 0;
}

BOOL window_fullscreen(HWND w) {
    RECT wr;
    if (!GetWindowRect(w, &wr)) return FALSE;
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return FALSE;
    const int T = 2;
    RECT r = mi.rcMonitor;
    return wr.left <= r.left + T && wr.top <= r.top + T &&
           wr.right >= r.right - T && wr.bottom >= r.bottom - T;
}

BOOL key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int measure_text_w(HFONT font, const wchar_t *s) {
    HDC hdc = GetDC(NULL);
    if (!hdc) return 0;
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE sz = {0, 0};
    int n = (int)wcslen(s);
    if (n > 0) GetTextExtentPoint32W(hdc, s, n, &sz);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
    return sz.cx;
}

HFONT make_font(int pt, int weight, int dpi) {
    int height = -(pt * dpi / 72);
    return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY,
                       VARIABLE_PITCH, L"Segoe UI");
}

int dpi_for_window(HWND h) {
    UINT d = GetDpiForWindow(h);
    return d ? (int)d : 96;
}

HWND create_child(HWND parent, const wchar_t *cls, const wchar_t *text,
                  DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND hwnd = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                                x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (font) SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    return hwnd;
}

void set_window_text_w(HWND h, const wchar_t *s) {
    SetWindowTextW(h, s);
}

void adjust_outer(int cw, int ch, DWORD style, int dpi, int *ow, int *oh) {
    RECT r = {0, 0, cw, ch};
    if (AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi)) {
        *ow = r.right - r.left;
        *oh = r.bottom - r.top;
    } else {
        *ow = cw + 16;
        *oh = ch + 39;
    }
}

void format_date(FILETIME ft, wchar_t *out, int outcch) {
    SYSTEMTIME st;
    if (FileTimeToSystemTime(&ft, &st))
        swprintf(out, outcch, L"%02d.%02d.%04d", st.wDay, st.wMonth, st.wYear);
    else
        out[0] = 0;
}

int days_until(FILETIME exp) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER e, n;
    e.LowPart = exp.dwLowDateTime;  e.HighPart = exp.dwHighDateTime;
    n.LowPart = now.dwLowDateTime;  n.HighPart = now.dwHighDateTime;
    long long diff = (long long)e.QuadPart - (long long)n.QuadPart; // 100ns
    return (int)(diff / (10000000LL * 86400LL));
}

void open_url(const wchar_t *url) {
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOW);
}

void open_help(void) {
    HRSRC hr = FindResourceW(NULL, L"HELPHTML", RT_RCDATA);
    if (!hr) return;
    HGLOBAL hg = LoadResource(NULL, hr);
    if (!hg) return;
    void *p = LockResource(hg);
    DWORD sz = SizeofResource(NULL, hr);
    if (!p || !sz) return;
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    swprintf(path, MAX_PATH, L"%lsrdpkey-help.html", tmp);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(f, p, sz, &wr, NULL);
    CloseHandle(f);
    open_url(path);
}

void msg_box(const wchar_t *text, const wchar_t *caption, UINT flags) {
    MessageBoxW(NULL, text, caption, flags);
}
