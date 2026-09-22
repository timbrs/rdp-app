// Ассоциация .rdp в «Открыть с помощью» (только HKCU, без прав админа, НЕ как
// приложение по умолчанию). Ассоциируем ТЕКУЩИЙ exe, себя не копируем. Идемпотентно.
#include "rdpkey.h"
#include <shlobj.h>
#include <wchar.h>
#include <stdio.h>

#define PROGID       L"rdpkey.rdp"
#define FRIENDLYNAME L"RDP RemoteApp Hotkeys"
#define PROGID_TITLE L"\x0423\x0434\x0430\x043b\x0451\x043d\x043d\x044b\x0439 \x0440\x0430\x0431\x043e\x0447\x0438\x0439 \x0441\x0442\x043e\x043b (rdpkey)"

static void reg_set_string(const wchar_t *path, const wchar_t *name, const wchar_t *value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value,
                   (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

void install_association(void) {
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) return;

    wchar_t cmd[MAX_PATH + 16];
    swprintf(cmd, MAX_PATH + 16, L"\"%ls\" \"%%1\"", exe);

    // Уже прописано на этот exe? — выходим сразу.
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\" PROGID L"\\shell\\open\\command",
            0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        wchar_t cur[MAX_PATH + 16];
        DWORD sz = sizeof(cur), ty = 0;
        LONG r = RegQueryValueExW(k, NULL, NULL, &ty, (BYTE *)cur, &sz);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && ty == REG_SZ && wcscmp(cur, cmd) == 0)
            return;
    }

    wchar_t iconVal[MAX_PATH + 4];
    swprintf(iconVal, MAX_PATH + 4, L"%ls,0", exe);

    reg_set_string(L"Software\\Classes\\" PROGID, L"", PROGID_TITLE);
    reg_set_string(L"Software\\Classes\\" PROGID L"\\DefaultIcon", L"", iconVal);
    reg_set_string(L"Software\\Classes\\" PROGID L"\\shell\\open\\command", L"", cmd);
    reg_set_string(L"Software\\Classes\\.rdp\\OpenWithProgids", PROGID, L"");
    reg_set_string(L"Software\\Classes\\Applications\\rdpkey.exe\\shell\\open\\command", L"", cmd);
    reg_set_string(L"Software\\Classes\\Applications\\rdpkey.exe", L"FriendlyAppName", FRIENDLYNAME);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}
