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
// RAIL-окно, прежде чем сдаться. Запущенный mstsc отдаёт подключение COM-процессу
// `mstsc -Embedding` и выходит через ~2 с, так что ждём по сути только окон —
// с запасом на долгий логин через VPN/брокер.
#define GIVEUP_TICKS 300
// Сколько тиков ждать после пропажи якоря, пока не найдётся новый полноэкранный
// RAIL (реконнект, пересоздание окна), прежде чем считать сеанс закрытым.
#define LOST_TICKS 15

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

// Предпочитаем наше RAIL-окно; иначе — любое полноэкранное (сеанс открыт до запуска).
static HWND s_fsRail, s_fsAny;
static BOOL CALLBACK enumFindFsRail(HWND h, LPARAM lp) {
    (void)lp;
    if (!IsWindowVisible(h) || !class_name_equal(h, L"RAIL_WINDOW")) return TRUE;
    if (!window_fullscreen(h)) return TRUE;
    if (!is_our_rail(h)) {
        if (!s_fsAny) s_fsAny = h;
        return TRUE;
    }
    s_fsRail = h;
    return FALSE;
}
HWND find_fullscreen_rail(void) {
    s_fsRail = s_fsAny = NULL;
    EnumWindows(enumFindFsRail, 0);
    return s_fsRail ? s_fsRail : s_fsAny;
}

#ifdef RDPKEY_LOG
static void log_tick(HWND prevBound, BOOL seen, int gone, HANDLE hMstsc, DWORD mstscPid) {
    static int n, lastSeen = -1, lastGone = -1, lastAlive = -2, lastOur = -1;
    static HWND lastBound = (HWND)-1;
    n++;
    int alive = -1;
    if (hMstsc) alive = WaitForSingleObject(hMstsc, 0) != WAIT_OBJECT_0;
    if (alive != lastAlive) {
        DWORD code = 0;
        if (alive == 0) GetExitCodeProcess(hMstsc, &code);
        dlog(L"запущенный mstsc pid=%lu: %ls (код выхода %lu)", mstscPid,
             alive == 1 ? L"жив" : alive == 0 ? L"ЗАВЕРШИЛСЯ" : L"нет хендла", code);
    }
    if (prevBound != g_boundRail) {
        if (prevBound && !IsWindow(prevBound)) dlog(L"якорь %p УНИЧТОЖЕН", prevBound);
        dlog_wnd(L"якорь теперь", g_boundRail);
    }
    int our = have_our_rail();
    BOOL changed = seen != lastSeen || gone != lastGone || our != lastOur ||
                   g_boundRail != lastBound || alive != lastAlive;
    if (changed || n % 30 == 0) {
        LASTINPUTINFO li;
        li.cbSize = sizeof(li);
        DWORD inputAge = GetLastInputInfo(&li) ? (GetTickCount() - li.dwTime) / 1000 : 0;
        long hookAge = g_hookTick ? (long)((GetTickCount64() - g_hookTick) / 1000) : -1;
        dlog(L"тик %d: якорь=%p seen=%d gone=%d mstsc=%d нашиRAIL=%d хук=%p | хук звали %ld с назад, ввод был %lu с назад",
             n, g_boundRail, seen, gone, alive, our, g_hook, hookAge, inputAge);
        dlog_wnd(L"  foreground", GetForegroundWindow());
    }
    dlog_windows(mstscPid);
    lastSeen = seen; lastGone = gone; lastAlive = alive; lastOur = our; lastBound = g_boundRail;
}
#endif

static void mstsc_path(wchar_t *out, int cch) {
    wchar_t windir[MAX_PATH];
    if (GetEnvironmentVariableW(L"windir", windir, MAX_PATH) == 0)
        wcscpy(windir, L"C:\\Windows");
    swprintf(out, cch, L"%ls\\System32\\mstsc.exe", windir);
}

void run_session(const wchar_t *rdp, Hotkeys hk) {
    gHotkeys = hk;
    DLOG(L"run_session: rdp=\"%ls\" хоткеи win=%d ctrlEsc=%d altTab=%d altShiftTab=%d prtsc=%d",
         rdp ? rdp : L"", hk.win, hk.ctrlEsc, hk.altTab, hk.altShiftTab, hk.printScreen);

    // Проверка персонального сертификата — при ЛЮБОМ подключении (в т.ч. по .rdp,
    // когда GUI не открывается).
    warn_if_personal_cert_expiring();
    DLOG(L"проверка сертификата пройдена");

    wchar_t mstsc[MAX_PATH];
    mstsc_path(mstsc, MAX_PATH);

    snapshot_baseline_rail();
    DLOG(L"базовых RAIL-окон до запуска mstsc: %d", g_baselineRailN);
    DLOG_WINDOWS(0);

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
        DLOG(L"ShellExecuteEx(mstsc) ОШИБКА %lu", GetLastError());
        msg_box(L"Не удалось запустить mstsc.exe.", L"rdpkey", MB_ICONERROR);
        return;
    }
    HANDLE hMstsc = sei.hProcess; // может быть NULL, если оболочка не вернула хендл
#ifdef RDPKEY_LOG
    DWORD mstscPid = hMstsc ? GetProcessId(hMstsc) : 0;
    dlog(L"mstsc запущен: %ls %ls, hProcess=%p pid=%lu", mstsc, params, hMstsc, mstscPid);
#endif

    if (!install_hook()) {
        DLOG(L"install_hook ОШИБКА %lu", GetLastError());
        msg_box(L"Не удалось установить LL-хук.", L"rdpkey", MB_ICONERROR);
        CloseHandle(hMstsc);
        return;
    }

    DLOG(L"LL-хук установлен: %p", g_hook);

    SetTimer(NULL, 1, 2000, NULL);
    BOOL seen = FALSE;
    int gone = 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
#ifdef RDPKEY_LOG
            HWND prevBound = g_boundRail;
#endif
            // Якорь — RAIL-окно нашей сессии. Пока живо (в т.ч. свёрнуто) — сессия
            // жива. Умерло — пробуем перепривязаться к свежему полноэкранному RAIL.
            if (g_boundRail == 0 || !IsWindow(g_boundRail))
                g_boundRail = find_fullscreen_rail();

            if (g_boundRail != 0) {
                seen = TRUE;
                gone = 0;
            } else if (seen) {
                gone++;
                if (gone >= LOST_TICKS) { // сессия была и пропала
                    DLOG(L"ВЫХОД: якорь пропал и нет полноэкранного RAIL %d тиков подряд", gone);
                    PostQuitMessage(0);
                }
            } else {
                // Сессии ещё не было: ждём логина (может тянуться 5+ минут). Живём,
                // пока наш mstsc жив ИЛИ уже есть наше RAIL-окно.
                BOOL mstscAlive = hMstsc ? (WaitForSingleObject(hMstsc, 0) != WAIT_OBJECT_0) : FALSE;
                BOOL connecting = mstscAlive || have_our_rail();
                if (connecting) gone = 0;
                else {
                    gone++;
                    if (gone >= GIVEUP_TICKS) {
                        DLOG(L"ВЫХОД: сеанс так и не появился, mstsc мёртв и нет наших RAIL %d тиков", gone);
                        PostQuitMessage(0);
                    }
                }
            }
#ifdef RDPKEY_LOG
            log_tick(prevBound, seen, gone, hMstsc, mstscPid);
#endif
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DLOG(L"цикл сообщений завершён (msg=0x%X), снимаю хук", msg.message);
    remove_hook();
    if (hMstsc) CloseHandle(hMstsc);
}
