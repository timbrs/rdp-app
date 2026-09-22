// Точка входа. Аргумент .rdp -> сразу сеанс без GUI; иначе — прописать ассоциацию
// и открыть GUI.
#include "rdpkey.h"
#include <shellapi.h>
#include <wchar.h>

HINSTANCE g_hInst;

static const wchar_t *arg_after_program(void) {
    static wchar_t buf[1024];
    buf[0] = 0;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2) {
        wcsncpy(buf, argv[1], 1023);
        buf[1023] = 0;
    }
    if (argv) LocalFree(argv);
    return buf[0] ? buf : NULL;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE prev, LPWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    g_hInst = hInst;

    Config cfg = load_config();
    ensure_config(&cfg); // молча создаём config.ini с дефолтами, если его ещё нет

    const wchar_t *arg = arg_after_program();
    if (arg) { // запуск с .rdp: сразу сеанс, без GUI
        run_session(arg, cfg.hotkeys);
        return 0;
    }

    install_association();

    const wchar_t *rdp = run_gui(&cfg);
    if (rdp && rdp[0])
        run_session(rdp, cfg.hotkeys);
    return 0;
}
