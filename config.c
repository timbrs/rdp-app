// Конфиг: чтение/запись config.ini в %LOCALAPPDATA%\rdpkey, раскрытие/сворачивание
// переменных окружения в путях, одноразовая миграция со старого config.json.
#include "rdpkey.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

static void appdata_dir(wchar_t *out, int cch) {
    wchar_t base[1024];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, 1024) == 0) base[0] = 0;
    swprintf(out, cch, L"%ls\\rdpkey", base);
}
static void config_path(wchar_t *out, int cch) {
    wchar_t d[1024];
    appdata_dir(d, 1024);
    swprintf(out, cch, L"%ls\\config.ini", d);
}
static void legacy_json_path(wchar_t *out, int cch) {
    wchar_t d[1024];
    appdata_dir(d, 1024);
    swprintf(out, cch, L"%ls\\config.json", d);
}

Config default_config(void) {
    Config c;
    c.hotkeys.win = c.hotkeys.ctrlEsc = c.hotkeys.altTab = c.hotkeys.altShiftTab = TRUE;
    c.hotkeys.winR = c.hotkeys.winE = c.hotkeys.winD = c.hotkeys.winTab = TRUE;
    c.hotkeys.winV = c.hotkeys.winZ = c.hotkeys.winShiftS = c.hotkeys.winOther = TRUE;
    c.hotkeys.printScreen = TRUE;
    c.lastRdpFile[0] = 0;
    return c;
}

void expand_env(const wchar_t *in, wchar_t *out, int outcch) {
    if (ExpandEnvironmentStringsW(in, out, outcch) == 0) {
        wcsncpy(out, in, outcch - 1);
        out[outcch - 1] = 0;
    }
}

// under_dir: p лежит внутри dir (по границе разделителя) -> rest = остаток.
static BOOL under_dir(const wchar_t *p, const wchar_t *dir, const wchar_t **rest) {
    int dl = (int)wcslen(dir);
    while (dl > 0 && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/')) dl--;
    if (dl == 0 || (int)wcslen(p) < dl) return FALSE;
    if (_wcsnicmp(p, dir, dl) != 0) return FALSE;
    wchar_t c = p[dl];
    if (c == 0 || c == L'\\' || c == L'/') { *rest = p + dl; return TRUE; }
    return FALSE;
}

void collapse_env(const wchar_t *in, wchar_t *out, int outcch) {
    if (in[0] == 0 || in[0] == L'%') {
        wcsncpy(out, in, outcch - 1); out[outcch - 1] = 0; return;
    }
    static const wchar_t *names[] = {L"OneDrive", L"LOCALAPPDATA", L"APPDATA", L"USERPROFILE", L"PUBLIC"};
    int bestIdx = -1, bestLen = -1;
    const wchar_t *bestRest = NULL;
    wchar_t val[1024];
    for (int i = 0; i < 5; i++) {
        if (GetEnvironmentVariableW(names[i], val, 1024) == 0) continue;
        const wchar_t *rest;
        if (under_dir(in, val, &rest)) {
            int l = (int)wcslen(val);
            if (l > bestLen) { bestLen = l; bestIdx = i; bestRest = rest; }
        }
    }
    if (bestIdx >= 0)
        swprintf(out, outcch, L"%%%ls%%%ls", names[bestIdx], bestRest);
    else {
        wcsncpy(out, in, outcch - 1); out[outcch - 1] = 0;
    }
}

// ---- чтение файла в wide (UTF-8 -> UTF-16, срез BOM) ----

static BOOL read_file_bytes(const wchar_t *path, char **out, int *outn) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD sz = GetFileSize(h, NULL);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return FALSE; }
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, sz, &rd, NULL);
    CloseHandle(h);
    if (!ok) { free(buf); return FALSE; }
    buf[rd] = 0;
    *out = buf; *outn = (int)rd;
    return TRUE;
}

static wchar_t *utf8_to_wide(const char *data, int n) {
    if (n >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        data += 3; n -= 3;
    }
    int wn = MultiByteToWideChar(CP_UTF8, 0, data, n, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((wn + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, data, n, w, wn);
    w[wn] = 0;
    return w;
}

static void lower_ascii(wchar_t *s) {
    for (; *s; s++) if (*s >= L'A' && *s <= L'Z') *s += 32;
}
static wchar_t *trim(wchar_t *s) {
    while (*s == L' ' || *s == L'\t' || *s == L'\r') s++;
    int n = (int)wcslen(s);
    while (n > 0 && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'\r')) s[--n] = 0;
    return s;
}
static BOOL parse_bool_w(const wchar_t *s) {
    wchar_t t[16];
    wcsncpy(t, s, 15); t[15] = 0;
    lower_ascii(t);
    return wcscmp(t, L"1") == 0 || wcscmp(t, L"true") == 0 ||
           wcscmp(t, L"yes") == 0 || wcscmp(t, L"on") == 0 ||
           wcscmp(t, L"\x0434\x0430") == 0; // "да"
}

static void set_hotkey(Hotkeys *h, const wchar_t *key, BOOL v) {
    if      (!wcscmp(key, L"win"))         h->win = v;
    else if (!wcscmp(key, L"ctrlesc"))     h->ctrlEsc = v;
    else if (!wcscmp(key, L"alttab"))      h->altTab = v;
    else if (!wcscmp(key, L"altshifttab")) h->altShiftTab = v;
    else if (!wcscmp(key, L"winr"))        h->winR = v;
    else if (!wcscmp(key, L"wine"))        h->winE = v;
    else if (!wcscmp(key, L"wind"))        h->winD = v;
    else if (!wcscmp(key, L"wintab"))      h->winTab = v;
    else if (!wcscmp(key, L"winv"))        h->winV = v;
    else if (!wcscmp(key, L"winz"))        h->winZ = v;
    else if (!wcscmp(key, L"winshifts"))   h->winShiftS = v;
    else if (!wcscmp(key, L"winother"))    h->winOther = v;
    else if (!wcscmp(key, L"printscreen")) h->printScreen = v;
}

// apply_ini: INI поверх cfg (отсутствующие ключи — как есть).
static void apply_ini(Config *cfg, const char *data, int n) {
    wchar_t *text = utf8_to_wide(data, n);
    if (!text) return;
    wchar_t section[64] = L"";
    wchar_t *p = text;
    while (*p) {
        wchar_t *nl = wcschr(p, L'\n');
        if (nl) *nl = 0;
        wchar_t *line = trim(p);
        if (line[0] && line[0] != L';' && line[0] != L'#') {
            int ll = (int)wcslen(line);
            if (line[0] == L'[' && line[ll-1] == L']') {
                line[ll-1] = 0;
                wchar_t *sec = trim(line + 1);
                wcsncpy(section, sec, 63); section[63] = 0;
                lower_ascii(section);
            } else {
                wchar_t *eq = wcschr(line, L'=');
                if (eq) {
                    *eq = 0;
                    wchar_t *key = trim(line);
                    wchar_t *val = trim(eq + 1);
                    lower_ascii(key);
                    if (!wcscmp(section, L"hotkeys"))
                        set_hotkey(&cfg->hotkeys, key, parse_bool_w(val));
                    else if (!wcscmp(section, L"general") && !wcscmp(key, L"lastrdpfile")) {
                        wcsncpy(cfg->lastRdpFile, val, 1023);
                        cfg->lastRdpFile[1023] = 0;
                    }
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(text);
}

// apply_json: минимальная миграция со старого config.json (структура
// {"Hotkeys":{"win":true,...},"LastRdpFile":"..."}). Толерантный скан, не парсер.
static BOOL json_flag(const wchar_t *t, const wchar_t *key, BOOL *out) {
    wchar_t pat[48];
    swprintf(pat, 48, L"\"%ls\"", key);
    const wchar_t *q = wcsstr(t, pat);
    if (!q) return FALSE;
    const wchar_t *tr = wcsstr(q, L"true");
    const wchar_t *fa = wcsstr(q, L"false");
    const wchar_t *comma = wcschr(q, L',');
    // ближайшее true/false после ключа (до следующей запятой не обязательно)
    if (tr && (!fa || tr < fa) && (!comma || tr < comma + 1)) { *out = TRUE; return TRUE; }
    if (fa) { *out = FALSE; return TRUE; }
    if (tr) { *out = TRUE; return TRUE; }
    return FALSE;
}
static void apply_json(Config *cfg, const char *data, int n) {
    wchar_t *t = utf8_to_wide(data, n);
    if (!t) return;
    BOOL v;
    if (json_flag(t, L"win", &v))         cfg->hotkeys.win = v;
    if (json_flag(t, L"ctrlEsc", &v))     cfg->hotkeys.ctrlEsc = v;
    if (json_flag(t, L"altTab", &v))      cfg->hotkeys.altTab = v;
    if (json_flag(t, L"altShiftTab", &v)) cfg->hotkeys.altShiftTab = v;
    if (json_flag(t, L"winR", &v))        cfg->hotkeys.winR = v;
    if (json_flag(t, L"winE", &v))        cfg->hotkeys.winE = v;
    if (json_flag(t, L"winD", &v))        cfg->hotkeys.winD = v;
    if (json_flag(t, L"winTab", &v))      cfg->hotkeys.winTab = v;
    if (json_flag(t, L"winV", &v))        cfg->hotkeys.winV = v;
    if (json_flag(t, L"winZ", &v))        cfg->hotkeys.winZ = v;
    if (json_flag(t, L"winShiftS", &v))   cfg->hotkeys.winShiftS = v;
    if (json_flag(t, L"winOther", &v))    cfg->hotkeys.winOther = v;
    if (json_flag(t, L"printScreen", &v)) cfg->hotkeys.printScreen = v;
    // LastRdpFile
    const wchar_t *q = wcsstr(t, L"\"LastRdpFile\"");
    if (q) {
        const wchar_t *c = wcschr(q, L':');
        if (c) {
            const wchar_t *o = wcschr(c, L'"');
            if (o) {
                o++;
                wchar_t out[1024]; int k = 0;
                while (*o && *o != L'"' && k < 1023) {
                    if (*o == L'\\' && o[1]) { o++; } // \\ -> \, \" -> "
                    out[k++] = *o++;
                }
                out[k] = 0;
                wcscpy(cfg->lastRdpFile, out);
            }
        }
    }
    free(t);
}

Config load_config(void) {
    Config cfg = default_config();
    wchar_t p[1024];
    char *data; int n;
    config_path(p, 1024);
    if (read_file_bytes(p, &data, &n)) { apply_ini(&cfg, data, n); free(data); return cfg; }
    legacy_json_path(p, 1024);
    if (read_file_bytes(p, &data, &n)) { apply_json(&cfg, data, n); free(data); }
    return cfg;
}

static void build_ini(const Config *cfg, wchar_t *out, int cch) {
    const Hotkeys *h = &cfg->hotkeys;
    wchar_t collapsed[1024];
    collapse_env(cfg->lastRdpFile, collapsed, 1024);
    swprintf(out, cch,
        L"; \x041d\x0430\x0441\x0442\x0440\x043e\x0439\x043a\x0438 rdpkey. "
        L"%%LOCALAPPDATA%%\\rdpkey\\config.ini\n"
        L"; \x041f\x0443\x0442\x0438 \x2014 \x0441 \x043e\x0434\x0438\x043d\x043e\x0447\x043d\x044b\x043c\x0438 \\ "
        L"\x0438 %%VAR%%, \x043d\x0430\x043f\x0440.: lastRdpFile=%%USERPROFILE%%\\Desktop\\work.rdp\n\n"
        L"[hotkeys]\n"
        L"win=%ls\nctrlEsc=%ls\naltTab=%ls\naltShiftTab=%ls\n"
        L"winR=%ls\nwinE=%ls\nwinD=%ls\nwinTab=%ls\nwinV=%ls\nwinZ=%ls\n"
        L"winShiftS=%ls\nwinOther=%ls\nprintScreen=%ls\n\n"
        L"[general]\nlastRdpFile=%ls\n",
        h->win?L"true":L"false", h->ctrlEsc?L"true":L"false",
        h->altTab?L"true":L"false", h->altShiftTab?L"true":L"false",
        h->winR?L"true":L"false", h->winE?L"true":L"false",
        h->winD?L"true":L"false", h->winTab?L"true":L"false",
        h->winV?L"true":L"false", h->winZ?L"true":L"false",
        h->winShiftS?L"true":L"false", h->winOther?L"true":L"false",
        h->printScreen?L"true":L"false", collapsed);
}

BOOL save_config(const Config *cfg) {
    wchar_t dir[1024];
    appdata_dir(dir, 1024);
    CreateDirectoryW(dir, NULL); // не страшно, если уже есть

    wchar_t content[4096];
    build_ini(cfg, content, 4096);

    int un = WideCharToMultiByte(CP_UTF8, 0, content, -1, NULL, 0, NULL, NULL);
    if (un <= 0) return FALSE;
    char *u8 = (char *)malloc(un + 3);
    if (!u8) return FALSE;
    u8[0] = (char)0xEF; u8[1] = (char)0xBB; u8[2] = (char)0xBF; // UTF-8 BOM
    WideCharToMultiByte(CP_UTF8, 0, content, -1, u8 + 3, un, NULL, NULL);
    int total = 3 + un - 1; // без завершающего нуля

    wchar_t tmp[1100];
    swprintf(tmp, 1100, L"%ls\\config.tmp", dir);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(u8); return FALSE; }
    DWORD wr; BOOL ok = WriteFile(h, u8, total, &wr, NULL);
    CloseHandle(h);
    free(u8);
    if (!ok) return FALSE;

    wchar_t dst[1100];
    config_path(dst, 1100);
    return MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING);
}

void ensure_config(const Config *cfg) {
    wchar_t p[1024];
    config_path(p, 1024);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES)
        save_config(cfg);
}
