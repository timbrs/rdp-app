// Блокирующее окно предупреждения: первые 5 секунд закрыть нельзя (кнопка неактивна,
// крестик игнорируется), таймер показывает остаток. '\x01' в тексте обрамляет
// красные фрагменты (дата, «истёк»).
#include "rdpkey.h"
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>

#define ERR_LOCK_SECONDS 5
#define ERR_RED 0x000000C8 // COLORREF (0x00BBGGRR): насыщенный красный, R=200

static HWND    hErrBtn, hErrCountdown;
static int     errRemaining;
static wchar_t errBody[4096];
static DWORD   errWndStyle;
static int     errDPI = 96;
static HBRUSH  errFaceBrush;
static HWND    errRedStatics[64];
static int     errRedN;

static int errSc(int v) { return v * errDPI / 96; }

static void countdownText(int sec, wchar_t *out, int cch) {
    if (sec <= 0) out[0] = 0;
    else swprintf(out, cch, L"Закрыть можно будет через %d с", sec);
}

static int measureTextHeight(HWND hwnd, HFONT font, const wchar_t *s, int width) {
    HDC hdc = GetDC(hwnd);
    if (!hdc) return width;
    HGDIOBJ old = SelectObject(hdc, font);
    int n = (int)wcslen(s), h = 0;
    if (n > 0) {
        RECT rc = {0, 0, width, 0};
        DrawTextW(hdc, s, n, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        h = rc.bottom - rc.top;
    }
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
    return h;
}

static void errOnCreate(HWND hwnd) {
    errRemaining = ERR_LOCK_SECONDS;
    errDPI = dpi_for_window(hwnd);
    errRedN = 0;
    HFONT font = make_font(10, FW_NORMAL, errDPI);
    errFaceBrush = GetSysColorBrush(COLOR_BTNFACE);

    const int w = 560;
    int textX = errSc(70), textY = errSc(22), textW = errSc(w - 92);
    int lineH = measureTextHeight(hwnd, font, L"Ag", errSc(4000));
    if (lineH <= 0) lineH = errSc(18);

    HICON icon = LoadIconW(NULL, IDI_WARNING);
    HWND hIco = create_child(hwnd, L"STATIC", L"", SS_ICON, errSc(22), errSc(24),
                             errSc(32), errSc(32), 0, NULL);
    SendMessageW(hIco, STM_SETICON, (WPARAM)icon, 0);

    int y = textY;
    wchar_t *body = _wcsdup(errBody);
    wchar_t *line = body;
    while (line) {
        wchar_t *nl = wcschr(line, L'\n');
        if (nl) *nl = 0;
        if (line[0] == 0) {
            y += lineH; // пустая строка — вертикальный отступ
        } else if (!wcschr(line, L'\x01')) {
            int h = measureTextHeight(hwnd, font, line, textW);
            if (h < lineH) h = lineH;
            create_child(hwnd, L"STATIC", line, 0, textX, y, textW, h, 0, font);
            y += h;
        } else {
            int x = textX, idx = 0;
            wchar_t *run = line;
            while (run) {
                wchar_t *m = wcschr(run, L'\x01');
                if (m) *m = 0;
                if (run[0]) {
                    int rw = measure_text_w(font, run) + errSc(2);
                    HWND hs = create_child(hwnd, L"STATIC", run, 0, x, y, rw, lineH, 0, font);
                    if ((idx % 2) == 1 && errRedN < 64) errRedStatics[errRedN++] = hs;
                    x += rw;
                }
                idx++;
                if (!m) break;
                run = m + 1;
            }
            y += lineH;
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(body);

    int rowY = y + errSc(16);
    hErrCountdown = create_child(hwnd, L"STATIC", L"", 0, errSc(22), rowY + errSc(6),
                                 errSc(260), errSc(20), 0, font);
    wchar_t ct[64];
    countdownText(errRemaining, ct, 64);
    set_window_text_w(hErrCountdown, ct);

    const int bw = 110;
    hErrBtn = create_child(hwnd, L"BUTTON", L"Закрыть",
                           BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED,
                           errSc(w - bw - 20), rowY, errSc(bw), errSc(30), 1, font);

    SetTimer(hwnd, 1, 1000, NULL);

    int clientH = rowY + errSc(30) + errSc(14);
    int ow, oh;
    adjust_outer(errSc(w), clientH, errWndStyle, errDPI, &ow, &oh);
    int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
    MoveWindow(hwnd, (scrW - ow) / 2, (scrH - oh) / 2, ow, oh, TRUE);
    SetForegroundWindow(hwnd);
}

static LRESULT CALLBACK errWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            errOnCreate(hwnd);
            return 0;
        case WM_TIMER: {
            errRemaining--;
            wchar_t ct[64];
            countdownText(errRemaining, ct, 64);
            set_window_text_w(hErrCountdown, ct);
            if (errRemaining <= 0) {
                KillTimer(hwnd, 1);
                EnableWindow(hErrBtn, TRUE);
            }
            return 0;
        }
        case WM_COMMAND:
            if (errRemaining <= 0) DestroyWindow(hwnd);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wParam, TRANSPARENT);
            for (int i = 0; i < errRedN; i++)
                if ((HWND)lParam == errRedStatics[i]) { SetTextColor((HDC)wParam, ERR_RED); break; }
            return (LRESULT)errFaceBrush;
        case WM_CLOSE:
            if (errRemaining <= 0) DestroyWindow(hwnd); // до конца отсчёта игнорируем
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void show_locked_error(const wchar_t *title, const wchar_t *body) {
    wcsncpy(errBody, body, 4095);
    errBody[4095] = 0;

    HINSTANCE hInst = g_hInst;
    HICON ico = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = errWndProc;
    wc.hInstance = hInst;
    wc.hIcon = ico;
    wc.hIconSm = ico;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"RdpKeyErrWnd";
    RegisterClassExW(&wc);

    errWndStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    HWND hwnd = CreateWindowExW(0, L"RdpKeyErrWnd", title, errWndStyle,
                                CW_USEDEFAULT, CW_USEDEFAULT, 480, 260,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) {
        msg_box(body, title, MB_ICONERROR);
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
