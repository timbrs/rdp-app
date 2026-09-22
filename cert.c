// Персональный клиентский сертификат (CurrentUser\MY, издатель ufa-ca01/ufa-ca02):
// за 30 дней до конца (и после) — предупреждаем при каждом подключении.
#include "rdpkey.h"

#ifdef NO_CERT
void warn_if_personal_cert_expiring(void) {} // вариант без crypt32 — тест ML-профиля
#else
#include <wincrypt.h>
#include <wchar.h>
#include <stdio.h>

#define WARN_DAYS 30

static BOOL issuer_is_ours(const wchar_t *iss) {
    wchar_t low[512];
    wcsncpy(low, iss, 511); low[511] = 0;
    for (wchar_t *p = low; *p; p++) if (*p >= L'A' && *p <= L'Z') *p += 32;
    return wcsstr(low, L"ufa-ca01") != NULL || wcsstr(low, L"ufa-ca02") != NULL;
}

static BOOL personal_cert_expiry(FILETIME *exp, wchar_t *name, int namecch) {
    HCERTSTORE hs = CertOpenSystemStoreW(0, L"MY");
    if (!hs) return FALSE;
    BOOL ok = FALSE;
    PCCERT_CONTEXT ctx = NULL;
    while ((ctx = CertEnumCertificatesInStore(hs, ctx)) != NULL) {
        wchar_t issuer[512];
        if (CertNameToStrW(X509_ASN_ENCODING, &ctx->pCertInfo->Issuer,
                           CERT_X500_NAME_STR, issuer, 512) == 0)
            continue;
        if (!issuer_is_ours(issuer)) continue;
        FILETIME na = ctx->pCertInfo->NotAfter;
        if (!ok || CompareFileTime(&na, exp) < 0) {
            *exp = na;
            name[0] = 0;
            CertGetNameStringW(ctx, CERT_NAME_ATTR_TYPE, 0,
                               (void *)szOID_COMMON_NAME, name, namecch);
            ok = TRUE;
        }
    }
    CertCloseStore(hs, 0);
    return ok;
}

static const wchar_t *plural_days(int n) {
    if (n < 0) n = -n;
    if (n % 100 >= 11 && n % 100 <= 14) return L"дней";
    switch (n % 10) {
        case 1: return L"день";
        case 2: case 3: case 4: return L"дня";
    }
    return L"дней";
}

// Общий «хвост» обоих сообщений.
static const wchar_t *service_desk =
    L"Для продолжения использования удалённого доступа оформите заявку в Service Desk.\n"
    L"Название заявки «Удалённый доступ к стационарному АРМ для сотрудников Банка».\n"
    L"\n"
    L"Контакты Service Desk:\n"
    L"8-495-785-12-12, 055-5555 (Москва)\n"
    L"8-347-279-66-55 (Уфа)\n"
    L"8-800-200-01-55 (Все города)";

// cert_warning_body: '\x01' обрамляет красные фрагменты (дата и «истёк»/«истечёт»).
static void cert_warning_body(FILETIME exp, const wchar_t *name, int days,
                              BOOL expired, wchar_t *out, int cch) {
    wchar_t date[32];
    format_date(exp, date, 32);
    if (expired) {
        swprintf(out, cch,
            L"Ваш персональный сертификат для удалённого доступа \x01истёк\x01.\n\n"
            L"Владелец: %ls\n"
            L"Срок действия: \x01%ls\x01\n\n"
            L"%ls",
            name, date, service_desk);
    } else {
        swprintf(out, cch,
            L"Ваш персональный сертификат для удалённого доступа скоро \x01истечёт\x01.\n\n"
            L"Владелец: %ls\n"
            L"Срок действия: \x01%ls\x01\n"
            L"Осталось: %d %ls\n\n"
            L"После этой даты удалённый доступ будет невозможен.\n\n"
            L"%ls",
            name, date, days, plural_days(days), service_desk);
    }
}

void warn_if_personal_cert_expiring(void) {
    FILETIME exp;
    wchar_t name[256];
    name[0] = 0;
    if (!personal_cert_expiry(&exp, name, 256)) return;

    int days = days_until(exp);
    if (days > WARN_DAYS) return;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    BOOL expired = CompareFileTime(&exp, &now) <= 0;
    if (name[0] == 0) wcscpy(name, L"—");

    wchar_t body[2048];
    cert_warning_body(exp, name, days, expired, body, 2048);
    show_locked_error(L"Срок действия сертификата удалённого доступа", body);
}
#endif // NO_CERT
