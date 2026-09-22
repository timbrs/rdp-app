// Чтение .rdp с учётом кодировки; извлечение полей; дата окончания подписи (.rdp
// подписан PKCS#7 в поле signature:s:). Разбор подписи — через CryptoAPI.
#include "rdpkey.h"
#include <wincrypt.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

static BOOL read_bytes(const wchar_t *path, unsigned char **out, int *outn) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD sz = GetFileSize(h, NULL);
    unsigned char *buf = (unsigned char *)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return FALSE; }
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, sz, &rd, NULL);
    CloseHandle(h);
    if (!ok) { free(buf); return FALSE; }
    buf[rd] = 0;
    *out = buf; *outn = (int)rd;
    return TRUE;
}

static wchar_t *decode_utf16(const unsigned char *b, int n, BOOL le) {
    int cnt = n / 2;
    wchar_t *w = (wchar_t *)malloc((cnt + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    for (int i = 0; i < cnt; i++) {
        unsigned c = le ? (b[2*i] | (b[2*i+1] << 8)) : ((b[2*i] << 8) | b[2*i+1]);
        w[i] = (wchar_t)c;
    }
    w[cnt] = 0;
    return w;
}

static wchar_t *decode_utf8(const unsigned char *b, int n) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, (const char *)b, n, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((wn + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, (const char *)b, n, w, wn);
    w[wn] = 0;
    return w;
}

BOOL read_rdp_text(const wchar_t *path, wchar_t **outText) {
    unsigned char *b; int n;
    if (!read_bytes(path, &b, &n)) return FALSE;
    wchar_t *w = NULL;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE)                 w = decode_utf16(b + 2, n - 2, TRUE);
    else if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF)           w = decode_utf16(b + 2, n - 2, FALSE);
    else if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) w = decode_utf8(b + 3, n - 3);
    else if (n >= 2 && b[1] == 0x00 && b[0] != 0x00)          w = decode_utf16(b, n, TRUE);
    else                                                       w = decode_utf8(b, n);
    free(b);
    if (!w) return FALSE;
    *outText = w;
    return TRUE;
}

// find_value_range: для строки «key:тип:значение» вернуть [vs,ve) значения (после
// второго ':'), без копирования (подпись — одна очень длинная строка).
static BOOL find_value_range(const wchar_t *text, const wchar_t *keyColon,
                             const wchar_t **vs, const wchar_t **ve) {
    int kl = (int)wcslen(keyColon);
    const wchar_t *p = text;
    while (*p) {
        const wchar_t *nl = wcschr(p, L'\n');
        const wchar_t *lineEnd = nl ? nl : p + wcslen(p);
        const wchar_t *q = p;
        while (q < lineEnd && (*q == L' ' || *q == L'\t')) q++;
        if ((lineEnd - q) >= kl && _wcsnicmp(q, keyColon, kl) == 0) {
            const wchar_t *t = q + kl;               // тип
            const wchar_t *colon = t;
            while (colon < lineEnd && *colon != L':') colon++;
            if (colon < lineEnd) {
                const wchar_t *v = colon + 1;
                const wchar_t *e = lineEnd;
                while (e > v && (e[-1] == L'\r' || e[-1] == L' ' || e[-1] == L'\t')) e--;
                *vs = v; *ve = e;
                return TRUE;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return FALSE;
}

BOOL rdp_value(const wchar_t *text, const wchar_t *key, wchar_t *out, int outcch) {
    wchar_t kc[128];
    swprintf(kc, 128, L"%ls:", key);
    const wchar_t *vs, *ve;
    if (!find_value_range(text, kc, &vs, &ve)) return FALSE;
    // ведущие пробелы значения тоже уберём (Go сравнивает TrimSpace)
    while (vs < ve && (*vs == L' ' || *vs == L'\t')) vs++;
    int len = (int)(ve - vs);
    if (len >= outcch) len = outcch - 1;
    wcsncpy(out, vs, len);
    out[len] = 0;
    return TRUE;
}

BOOL is_remoteapp_rdp(const wchar_t *text) {
    wchar_t v[16];
    if (!rdp_value(text, L"remoteapplicationmode", v, 16)) return FALSE;
    return wcscmp(v, L"1") == 0;
}

#ifdef NO_CERT
BOOL expiry_from_text(const wchar_t *text, FILETIME *out) { (void)text; (void)out; return FALSE; }
#else
// signature_expiry: base64 -> blob (12-байт заголовок rdpsign + PKCS#7). Находим
// ContentInfo по OID signedData, грузим PKCS#7 через CryptQueryObject и берём
// NotAfter именно подписавшего сертификата (CMSG_SIGNER_CERT_INFO_PARAM).
static BOOL signature_expiry(const char *b64, FILETIME *out) {
    DWORD blen = 0;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, NULL, &blen, NULL, NULL) || blen == 0)
        return FALSE;
    BYTE *blob = (BYTE *)malloc(blen);
    if (!blob) return FALSE;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, blob, &blen, NULL, NULL)) {
        free(blob);
        return FALSE;
    }

    static const BYTE oid[] = {0x06,0x09,0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x07,0x02};
    int idx = -1;
    for (DWORD i = 0; i + sizeof(oid) <= blen; i++) {
        if (memcmp(blob + i, oid, sizeof(oid)) == 0) { idx = (int)i; break; }
    }
    if (idx < 0) { free(blob); return FALSE; }

    BOOL ok = FALSE;
    for (int hdr = 2; hdr <= 7 && !ok; hdr++) {
        int start = idx - hdr;
        if (start < 0 || blob[start] != 0x30) continue;
        CERT_BLOB cb;
        cb.cbData = blen - start;
        cb.pbData = blob + start;
        HCERTSTORE hStore = NULL;
        HCRYPTMSG hMsg = NULL;
        if (CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &cb,
                CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED, CERT_QUERY_FORMAT_FLAG_BINARY,
                0, NULL, NULL, NULL, &hStore, &hMsg, NULL)) {
            DWORD need = 0;
            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_CERT_INFO_PARAM, 0, NULL, &need) && need > 0) {
                CERT_INFO *ci = (CERT_INFO *)malloc(need);
                if (ci && CryptMsgGetParam(hMsg, CMSG_SIGNER_CERT_INFO_PARAM, 0, ci, &need)) {
                    PCCERT_CONTEXT signer = CertGetSubjectCertificateFromStore(
                        hStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, ci);
                    if (signer) {
                        *out = signer->pCertInfo->NotAfter;
                        ok = TRUE;
                        CertFreeCertificateContext(signer);
                    }
                }
                free(ci);
            }
            if (hMsg) CryptMsgClose(hMsg);
            if (hStore) CertCloseStore(hStore, 0);
        }
    }
    free(blob);
    return ok;
}

BOOL expiry_from_text(const wchar_t *text, FILETIME *out) {
    const wchar_t *vs, *ve;
    if (!find_value_range(text, L"signature:", &vs, &ve)) return FALSE;
    int cap = (int)(ve - vs);
    if (cap <= 0) return FALSE;
    char *b64 = (char *)malloc(cap + 1);
    if (!b64) return FALSE;
    int k = 0;
    for (const wchar_t *p = vs; p < ve; p++) {
        wchar_t c = *p;
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        if (c < 128) b64[k++] = (char)c;
    }
    b64[k] = 0;
    BOOL ok = (k > 0) && signature_expiry(b64, out);
    free(b64);
    return ok;
}
#endif // NO_CERT
