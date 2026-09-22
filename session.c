// Запуск mstsc, установка LL-хука, цикл сообщений со сторожем живучести. GUI к
// этому моменту разрушен — процесс живёт скрыто и завершается вместе с сеансом.
// Живучесть логина определяется по ОКНАМ (наш mstsc жив ИЛИ есть наше RAIL-окно) —
// без перебора всех процессов (это лишний «малварный» сигнал для ML-эвристик).
#include "rdpkey.h"
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdio.h>

// Сколько тиков (по 2 с) ждать после выхода нашего mstsc, пока не появилось наше
// RAIL-окно, прежде чем сдаться (запас на долгий логин/хендофф брокеру).
#define GIVEUP_TICKS 30

HWND *g_baselineRail = NULL;
int   g_baselineRailN = 0;

static HWND *s_collect;
static int   s_collectN, s_collectCap;

static BOOL CALLBACK enumCollectRail(HWND h, LPARAM lp) {
    (void)lp;
    if (class_name_equal(h, L"RAIL_WINDOW")) {
        if (s_collectN >= s_collectCap) {
            s_collectCap = s_collectCap ? s_collectCap * 2 : 16;
            s_collect = (HWND *)realloc(s_collect, s_collectCap * sizeof(HWND));
        }
        s_collect[s_collectN++] = h;
    }
    return TRUE;
}

static void snapshot_baseline_rail(void) {
    s_collect = NULL; s_collectN = 0; s_collectCap = 0;
    EnumWindows(enumCollectRail, 0);
    g_baselineRail = s_collect;
    g_baselineRailN = s_collectN;
}

BOOL is_our_rail(HWND h) {
    for (int i = 0; i < g_baselineRailN; i++)
        if (g_baselineRail[i] == h) return FALSE;
    return TRUE;
}

static BOOL s_haveOur;
static BOOL CALLBACK enumHaveOur(HWND h, LPARAM lp) {
    (void)lp;
    if (class_name_equal(h, L"RAIL_WINDOW") && is_our_rail(h)) { s_haveOur = TRUE; return FALSE; }
    return TRUE;
}
BOOL have_our_rail(void) {
    s_haveOur = FALSE;
    EnumWindows(enumHaveOur, 0);
    return s_haveOur;
}

static HWND s_fsRail;
static BOOL CALLBACK enumFindFsRail(HWND h, LPARAM lp) {
    (void)lp;
    if (!IsWindowVisible(h) || !class_name_equal(h, L"RAIL_WINDOW")) return TRUE;
    if (!is_our_rail(h)) return TRUE;
    if (!window_fullscreen(h)) return TRUE;
    s_fsRail = h;
    return FALSE;
}
HWND find_fullscreen_rail(void) {
    s_fsRail = NULL;
    EnumWindows(enumFindFsRail, 0);
    return s_fsRail;
}

static void mstsc_path(wchar_t *out, int cch) {
    wchar_t windir[MAX_PATH];
    if (GetEnvironmentVariableW(L"windir", windir, MAX_PATH) == 0)
        wcscpy(windir, L"C:\\Windows");
    swprintf(out, cch, L"%ls\\System32\\mstsc.exe", windir);
}

void run_session(const wchar_t *rdp, Hotkeys hk) {
    gHotkeys = hk;

    // Проверка персонального сертификата — при ЛЮБОМ подключении (в т.ч. по .rdp,
    // когда GUI не открывается).
    warn_if_personal_cert_expiring();

    wchar_t mstsc[MAX_PATH];
    mstsc_path(mstsc, MAX_PATH);

    snapshot_baseline_rail();

    // Запуск mstsc через оболочку («открыть программу»), а не CreateProcess: более
    // прикладной силуэт для эвристик (не «лоадер»), и статический импорт —
    // ShellExecuteExW вместо CreateProcessW.
    wchar_t params[1200];
    params[0] = 0;
    if (rdp && rdp[0]) swprintf(params, 1200, L"\"%ls\"", rdp);

    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = mstsc;
    sei.lpParameters = (rdp && rdp[0]) ? params : NULL;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        msg_box(L"Не удалось запустить mstsc.exe.", L"rdpkey", MB_ICONERROR);
        return;
    }
    HANDLE hMstsc = sei.hProcess; // может быть NULL, если оболочка не вернула хендл

    if (!install_hook()) {
        msg_box(L"Не удалось установить LL-хук.", L"rdpkey", MB_ICONERROR);
        CloseHandle(hMstsc);
        return;
    }

    SetTimer(NULL, 1, 2000, NULL);
    BOOL seen = FALSE;
    int gone = 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            // Якорь — RAIL-окно нашей сессии. Пока живо (в т.ч. свёрнуто) — сессия
            // жива. Умерло — пробуем перепривязаться к свежему полноэкранному RAIL.
            if (g_boundRail == 0 || !IsWindow(g_boundRail))
                g_boundRail = find_fullscreen_rail();

            if (g_boundRail != 0) {
                seen = TRUE;
                gone = 0;
            } else if (seen) {
                gone++;
                if (gone >= 2) PostQuitMessage(0); // сессия была и пропала
            } else {
                // Сессии ещё не было: ждём логина (может тянуться 5+ минут). Живём,
                // пока наш mstsc жив ИЛИ уже есть наше RAIL-окно.
                BOOL mstscAlive = hMstsc ? (WaitForSingleObject(hMstsc, 0) != WAIT_OBJECT_0) : FALSE;
                BOOL connecting = mstscAlive || have_our_rail();
                if (connecting) gone = 0;
                else {
                    gone++;
                    if (gone >= GIVEUP_TICKS) PostQuitMessage(0);
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    remove_hook();
    if (hMstsc) CloseHandle(hMstsc);
}
