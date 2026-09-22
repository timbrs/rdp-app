// rdpkey (C-порт) — проброс горячих клавиш в полноэкранный RemoteApp-сеанс RDP.
// Общий заголовок: конфиг, хоткеи, прототипы, разделяемые Win32-помощники.
#ifndef RDPKEY_H
#define RDPKEY_H

// Таргет Windows 10 1703+ — нужен для GetDpiForWindow / AdjustWindowRectExForDpi /
// SetProcessDpiAwarenessContext и типа DPI_AWARENESS_CONTEXT.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000003
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define APP_VERSION L"1.0.11"

// ---- конфиг ----

typedef struct {
    BOOL win, ctrlEsc, altTab, altShiftTab;
    BOOL winR, winE, winD, winTab, winV, winZ, winShiftS, winOther;
    BOOL printScreen;
} Hotkeys;

typedef struct {
    Hotkeys hotkeys;
    wchar_t lastRdpFile[1024]; // СЫРОЕ значение (может содержать %VAR%)
} Config;

// config.c
Config default_config(void);
Config load_config(void);
void   ensure_config(const Config *cfg);
BOOL   save_config(const Config *cfg);
void   expand_env(const wchar_t *in, wchar_t *out, int outcch);   // %VAR% -> значение
void   collapse_env(const wchar_t *in, wchar_t *out, int outcch); // путь -> %VAR%\...

// assoc.c
void install_association(void);

// rdp.c
BOOL read_rdp_text(const wchar_t *path, wchar_t **outText); // *outText -> free()
BOOL is_remoteapp_rdp(const wchar_t *text);
BOOL rdp_value(const wchar_t *text, const wchar_t *key, wchar_t *out, int outcch);
BOOL expiry_from_text(const wchar_t *text, FILETIME *out); // дата окончания подписи

// cert.c
void warn_if_personal_cert_expiring(void);

// hook.c — установка хука и состояние (используется session.c)
extern Hotkeys  gHotkeys;
extern HHOOK    g_hook;
BOOL install_hook(void);
void remove_hook(void);
void release_sticky(void);
void reset_win_state(void);

// hook.c/session.c — общие оконные помощники и состояние привязки
extern HWND g_targetWnd; // куда шлём проброшенные клавиши (IHWindowClass)
extern HWND g_boundRail; // якорь живучести — RAIL-окно нашей сессии
BOOL is_remote_focused(void);

// session.c
extern HWND *g_baselineRail;   // RAIL-окна, жившие ДО нашего запуска
extern int   g_baselineRailN;
BOOL is_our_rail(HWND h);
BOOL have_our_rail(void);
HWND find_fullscreen_rail(void);
void run_session(const wchar_t *rdp, Hotkeys hk);

// gui.c
const wchar_t *run_gui(Config *cfg); // выбранный .rdp или NULL

// errdialog.c
void show_locked_error(const wchar_t *title, const wchar_t *body);

// winutil.c — разделяемые Win32-обёртки
BOOL  class_name_equal(HWND h, const wchar_t *want);
BOOL  window_fullscreen(HWND h);
BOOL  key_down(int vk);
int   measure_text_w(HFONT font, const wchar_t *s);
HFONT make_font(int pt, int weight, int dpi);
int   dpi_for_window(HWND h);
HWND  create_child(HWND parent, const wchar_t *cls, const wchar_t *text,
                   DWORD style, int x, int y, int w, int h, int id, HFONT font);
void  set_window_text_w(HWND h, const wchar_t *s);
void  adjust_outer(int clientW, int clientH, DWORD style, int dpi, int *ow, int *oh);
void  format_date(FILETIME ft, wchar_t *out, int outcch); // ДД.ММ.ГГГГ (UTC)
int   days_until(FILETIME exp);                           // (exp - сейчас) в днях
void  open_url(const wchar_t *url);
void  open_help(void);
void  msg_box(const wchar_t *text, const wchar_t *caption, UINT flags);

// main.c
extern HINSTANCE g_hInst;

// Клавиши-модификаторы (маски).
enum { KM_ALT = 1, KM_CTRL = 2, KM_SHIFT = 4, KM_WIN = 8 };

#endif // RDPKEY_H
