// Главное окно: кнопка запуска, строка «Подключение к: …» с датой окончания файла,
// чекбоксы хоткеев, «Как пользоваться?».
#include "rdpkey.h"
#include <commctrl.h>
#include <commdlg.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>

#define ID_LAUNCH 100
#define ID_HELP   104
#define ID_CHANGE 106

typedef struct {
    int            id;
    const wchar_t *label;
    BOOL          *field;
    BOOL           indent;
    BOOL           winSub; // деактивируется при снятой мастер-галке «Win»
    BOOL           master;
    HWND           hwnd;
} HkItem;

static Config *gCfgPtr;
static wchar_t gGuiResult[1024];
static DWORD   gWndStyle;
static int     gDPI = 96;
static HWND    hMainWnd;
static HBRUSH  faceBrush;
static HFONT   bigFont, guiFont;

static BOOL    gHasConn;
static HWND    hConnStatic, hChangeBtn, hTooltip;
static BOOL    gExpiryWarn;
static int     gConnX, gConnY, gConnH;
static int     gChangeW;
static wchar_t gTipBuf[1024];

static HkItem  hkItems[16];
static int     hkItemsN;

static int scDPI(int v) { return v * gDPI / 96; }

static const wchar_t *lastRdp(void) {
    static wchar_t b[1024];
    expand_env(gCfgPtr->lastRdpFile, b, 1024);
    return b;
}

static void buildHkItems(Config *cfg) {
    Hotkeys *h = &cfg->hotkeys;
    int i = 0;
#define ADD(ID, LBL, FLD, IND, SUB, MST) do { \
        hkItems[i].id = (ID); hkItems[i].label = (LBL); hkItems[i].field = (FLD); \
        hkItems[i].indent = (IND); hkItems[i].winSub = (SUB); hkItems[i].master = (MST); \
        hkItems[i].hwnd = NULL; i++; } while (0)
    ADD(200, L"Win — открыть «Пуск» в сессии",             &h->win,         FALSE, FALSE, TRUE);
    ADD(201, L"Win+R — «Выполнить»",                        &h->winR,        TRUE,  TRUE,  FALSE);
    ADD(202, L"Win+E — Проводник",                          &h->winE,        TRUE,  TRUE,  FALSE);
    ADD(203, L"Win+D — показать рабочий стол",              &h->winD,        TRUE,  TRUE,  FALSE);
    ADD(204, L"Win+Tab — представление задач",              &h->winTab,      TRUE,  TRUE,  FALSE);
    ADD(205, L"Win+V — журнал буфера обмена",               &h->winV,        TRUE,  TRUE,  FALSE);
    ADD(211, L"Win+Shift+S — скриншот области (Ножницы)",   &h->winShiftS,   TRUE,  TRUE,  FALSE);
    ADD(210, L"Win+Z — свернуть удалёнку (локально)",       &h->winZ,        TRUE,  TRUE,  FALSE);
    ADD(206, L"Win+<прочие клавиши>",                       &h->winOther,    TRUE,  TRUE,  FALSE);
    ADD(207, L"Ctrl+Esc — открыть «Пуск»",                  &h->ctrlEsc,     FALSE, FALSE, FALSE);
    ADD(208, L"Alt+Tab — переключение окон",                &h->altTab,      FALSE, FALSE, FALSE);
    ADD(209, L"Alt+Shift+Tab — переключение назад",         &h->altShiftTab, FALSE, FALSE, FALSE);
    ADD(212, L"PrintScreen — скриншот всего экрана",        &h->printScreen, FALSE, FALSE, FALSE);
#undef ADD
    hkItemsN = i;
}

static void initDPIAware(void) {
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(-4)); // PER_MONITOR_AWARE_V2
}
static void initCommonControls(void) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);
}

static BOOL file_exists(const wchar_t *p) {
    if (!p || !p[0]) return FALSE;
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void updateWinSubEnable(void) {
    BOOL on = FALSE;
    for (int i = 0; i < hkItemsN; i++)
        if (hkItems[i].master) { on = *hkItems[i].field; break; }
    for (int i = 0; i < hkItemsN; i++)
        if (hkItems[i].winSub) EnableWindow(hkItems[i].hwnd, on);
}

static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *b = p;
    for (const wchar_t *q = p; *q; q++)
        if (*q == L'\\' || *q == L'/') b = q + 1;
    return b;
}

static void connInfo(const wchar_t *rdpText, wchar_t *label, int labelcch,
                     BOOL *warn, wchar_t *tip, int tipcch) {
    swprintf(label, labelcch, L"Подключение к: %ls", base_name(lastRdp()));
    *warn = FALSE;
    tip[0] = 0;
    FILETIME exp;
    if (expiry_from_text(rdpText, &exp)) {
        wchar_t d[32];
        format_date(exp, d, 32);
        int used = (int)wcslen(label);
        swprintf(label + used, labelcch - used, L", до %ls", d);
        if (days_until(exp) <= 30) {
            *warn = TRUE;
            swprintf(tip, tipcch,
                L"Срок действия файла удалёнки истекает %ls. После этой даты подключение "
                L"перестанет работать — запросите новый файл в службе поддержки.", d);
        }
    }
}

static void layoutConnRow(const wchar_t *label, int *staticW, int *btnX) {
    if (gChangeW == 0)
        gChangeW = measure_text_w(guiFont, L"изменить") + scDPI(20);
    int maxRight = scDPI(20 + 420);
    int avail = maxRight - gConnX - scDPI(10) - gChangeW;
    if (avail < scDPI(60)) avail = scDPI(60);
    int sw = measure_text_w(guiFont, label) + scDPI(4);
    if (sw > avail) sw = avail;
    *staticW = sw;
    *btnX = gConnX + sw + scDPI(10);
}

static void setupTooltip(HWND hwnd, const wchar_t *tip, BOOL warn, BOOL add) {
    if (!hTooltip) {
        hTooltip = CreateWindowExW(0, TOOLTIPS_CLASSW, L"",
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, NULL, g_hInst, NULL);
    }
    if (!hTooltip) return;
    wcsncpy(gTipBuf, tip ? tip : L"", 1023);
    gTipBuf[1023] = 0;
    TOOLINFOW ti;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = hwnd;
    ti.uId = (UINT_PTR)hConnStatic;
    ti.hinst = g_hInst;
    ti.lpszText = gTipBuf;
    SendMessageW(hTooltip, add ? TTM_ADDTOOLW : TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    SendMessageW(hTooltip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)scDPI(320));
    SendMessageW(hTooltip, TTM_ACTIVATE, (WPARAM)(warn ? TRUE : FALSE), 0);
}

static void createConnRow(HWND hwnd) {
    gConnX = scDPI(20);
    gConnY = scDPI(116);
    gConnH = scDPI(20);
    wchar_t *rdpText = NULL;
    read_rdp_text(lastRdp(), &rdpText);
    wchar_t label[512], tip[512];
    BOOL warn;
    connInfo(rdpText ? rdpText : L"", label, 512, &warn, tip, 512);
    gExpiryWarn = warn;
    int staticW, btnX;
    layoutConnRow(label, &staticW, &btnX);
    hConnStatic = create_child(hwnd, L"STATIC", label, SS_NOTIFY | SS_ENDELLIPSIS,
                               gConnX, gConnY, staticW, gConnH, 0, guiFont);
    hChangeBtn = create_child(hwnd, L"BUTTON", L"изменить", BS_PUSHBUTTON | WS_TABSTOP,
                              btnX, gConnY - scDPI(3), gChangeW, scDPI(24), ID_CHANGE, guiFont);
    setupTooltip(hwnd, tip, warn, TRUE);
    if (rdpText) free(rdpText);
}

static void applyConnRow(HWND hwnd, const wchar_t *rdpText) {
    if (!gHasConn) return;
    wchar_t label[512], tip[512];
    BOOL warn;
    connInfo(rdpText, label, 512, &warn, tip, 512);
    gExpiryWarn = warn;
    set_window_text_w(hConnStatic, label);
    int staticW, btnX;
    layoutConnRow(label, &staticW, &btnX);
    MoveWindow(hConnStatic, gConnX, gConnY, staticW, gConnH, TRUE);
    MoveWindow(hChangeBtn, btnX, gConnY - scDPI(3), gChangeW, scDPI(24), TRUE);
    setupTooltip(hwnd, tip, warn, FALSE);
    InvalidateRect(hConnStatic, NULL, TRUE);
}

static void onCreate(HWND hwnd) {
    gDPI = dpi_for_window(hwnd);
    bigFont = make_font(14, 600, gDPI);
    guiFont = make_font(9, FW_NORMAL, gDPI);

    const int margin = 20, btnW = 420;

    create_child(hwnd, L"STATIC", L"rdpkey v" APP_VERSION, 0,
                 scDPI(margin), scDPI(15), scDPI(180), scDPI(20), 0, guiFont);
    create_child(hwnd, L"BUTTON", L"Запустить удалёнку с пробросом клавиш",
                 BS_PUSHBUTTON | WS_TABSTOP, scDPI(margin), scDPI(46),
                 scDPI(btnW), scDPI(56), ID_LAUNCH, bigFont);

    int y = 116;
    gHasConn = file_exists(lastRdp());
    if (gHasConn) { createConnRow(hwnd); y = 150; }

    for (int i = 0; i < hkItemsN; i++) {
        HkItem *it = &hkItems[i];
        int x = margin, w = btnW;
        if (it->indent) { x += 22; w -= 22; }
        it->hwnd = create_child(hwnd, L"BUTTON", it->label,
                                BS_AUTOCHECKBOX | WS_TABSTOP,
                                scDPI(x), scDPI(y), scDPI(w), scDPI(22), it->id, guiFont);
        SendMessageW(it->hwnd, BM_SETCHECK, *it->field ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 25;
    }
    y += 6;
    create_child(hwnd, L"BUTTON", L"Как пользоваться?", BS_PUSHBUTTON | WS_TABSTOP,
                 scDPI(margin), scDPI(y), scDPI(200), scDPI(26), ID_HELP, guiFont);
    int clientH = y + 26 + 14;

    updateWinSubEnable();

    int ow, oh;
    adjust_outer(scDPI(margin * 2 + btnW), scDPI(clientH), gWndStyle, gDPI, &ow, &oh);
    int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
    MoveWindow(hwnd, (scrW - ow) / 2, (scrH - oh) / 2, ow, oh, TRUE);
}

static BOOL openRdpDialog(HWND hwnd, const wchar_t *initial, wchar_t *out, int outcch) {
    wchar_t buf[1024];
    buf[0] = 0;
    if (initial && initial[0]) { wcsncpy(buf, initial, 1023); buf[1023] = 0; }
    static const wchar_t filter[] =
        L"RDP-файлы (*.rdp)\0*.rdp\0Все файлы (*.*)\0*.*\0";
    wchar_t initDir[MAX_PATH];
    initDir[0] = 0;
    if (initial && initial[0]) {
        wcsncpy(initDir, initial, MAX_PATH - 1);
        initDir[MAX_PATH - 1] = 0;
        wchar_t *b = initDir;
        for (wchar_t *p = initDir; *p; p++) if (*p == L'\\' || *p == L'/') b = p;
        *b = 0;
    }
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"Выберите файл удаленки от работодателя";
    ofn.lpstrDefExt = L"rdp";
    ofn.lpstrInitialDir = (initial && initial[0]) ? initDir : NULL;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return FALSE;
    wcsncpy(out, buf, outcch - 1);
    out[outcch - 1] = 0;
    return TRUE;
}

static BOOL selectRdpFile(HWND hwnd, const wchar_t *initial,
                          wchar_t *outPath, int outcch, wchar_t **outText) {
    wchar_t p[1024];
    if (!openRdpDialog(hwnd, initial, p, 1024)) return FALSE;
    wchar_t *t = NULL;
    if (!read_rdp_text(p, &t)) {
        msg_box(L"Не удалось прочитать выбранный файл.", L"rdpkey", MB_ICONERROR);
        return FALSE;
    }
    if (!is_remoteapp_rdp(t)) {
        msg_box(L"Это не файл удалёнки в режиме RemoteApp.\n\n"
                L"Выберите .rdp-файл удалёнки, выданный работодателем.",
                L"rdpkey — неподходящий файл", MB_ICONERROR);
        free(t);
        return FALSE;
    }
    wcsncpy(outPath, p, outcch - 1);
    outPath[outcch - 1] = 0;
    *outText = t;
    return TRUE;
}

static void launchOrPick(HWND hwnd) {
    const wchar_t *last = lastRdp();
    if (file_exists(last)) {
        wchar_t *text = NULL;
        if (read_rdp_text(last, &text)) {
            BOOL ra = is_remoteapp_rdp(text);
            free(text);
            if (ra) {
                wcsncpy(gGuiResult, last, 1023);
                gGuiResult[1023] = 0;
                DestroyWindow(hwnd);
                return;
            }
        }
    }
    wchar_t p[1024], *text = NULL;
    if (selectRdpFile(hwnd, last, p, 1024, &text)) {
        wcsncpy(gCfgPtr->lastRdpFile, p, 1023);
        gCfgPtr->lastRdpFile[1023] = 0;
        wcsncpy(gGuiResult, p, 1023);
        gGuiResult[1023] = 0;
        save_config(gCfgPtr);
        free(text);
        DestroyWindow(hwnd);
    }
}

static void onCommand(HWND hwnd, WPARAM wParam) {
    int id = (int)LOWORD(wParam);
    if (id == ID_LAUNCH) {
        launchOrPick(hwnd);
    } else if (id == ID_CHANGE) {
        wchar_t p[1024], *text = NULL;
        if (selectRdpFile(hwnd, lastRdp(), p, 1024, &text)) {
            wcsncpy(gCfgPtr->lastRdpFile, p, 1023);
            gCfgPtr->lastRdpFile[1023] = 0;
            save_config(gCfgPtr);
            applyConnRow(hwnd, text);
            free(text);
        }
    } else if (id == ID_HELP) {
        open_help();
    } else {
        for (int i = 0; i < hkItemsN; i++) {
            if (hkItems[i].id == id) {
                HkItem *it = &hkItems[i];
                *it->field = SendMessageW(it->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (it->master) updateWinSubEnable();
                save_config(gCfgPtr);
                break;
            }
        }
    }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:  onCreate(hwnd); return 0;
        case WM_COMMAND: onCommand(hwnd, wParam); return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wParam, TRANSPARENT);
            if (gExpiryWarn && (HWND)lParam == hConnStatic)
                SetTextColor((HDC)wParam, RGB(255, 0, 0));
            return (LRESULT)faceBrush;
        case WM_CTLCOLORBTN:
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)faceBrush;
        case WM_CLOSE:   DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

const wchar_t *run_gui(Config *cfg) {
    gCfgPtr = cfg;
    gGuiResult[0] = 0;
    buildHkItems(cfg);
    initDPIAware();
    initCommonControls();

    HINSTANCE hInst = g_hInst;
    faceBrush = GetSysColorBrush(COLOR_BTNFACE);
    HICON ico = LoadIconW(hInst, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hIcon = ico;
    wc.hIconSm = ico;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"RdpKeyMainWnd";
    RegisterClassExW(&wc);

    gWndStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    hMainWnd = CreateWindowExW(0, L"RdpKeyMainWnd", L"rdpkey — проброс горячих клавиш",
                               gWndStyle, CW_USEDEFAULT, CW_USEDEFAULT, 480, 460,
                               NULL, NULL, hInst, NULL);
    if (!hMainWnd) return L"";
    ShowWindow(hMainWnd, SW_SHOW);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return gGuiResult;
}
