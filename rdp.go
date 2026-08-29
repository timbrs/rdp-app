package main

import (
	"bufio"
	"bytes"
	"crypto/x509"
	"encoding/asn1"
	"encoding/base64"
	"errors"
	"fmt"
	"math/big"
	"os"
	"strings"
	"time"
	"unicode/utf16"
)

// readRdpText читает .rdp с учётом кодировки. mstsc пишет UTF-16LE (BOM FF FE),
// но встречаются UTF-8 (в т.ч. с BOM), а изредка — UTF-16LE без BOM (после
// правки сторонним редактором). Ключи всегда ASCII.
func readRdpText(path string) (string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	switch {
	case len(b) >= 2 && b[0] == 0xFF && b[1] == 0xFE: // UTF-16LE BOM
		return decodeUTF16(b[2:], true), nil
	case len(b) >= 2 && b[0] == 0xFE && b[1] == 0xFF: // UTF-16BE BOM
		return decodeUTF16(b[2:], false), nil
	case len(b) >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF: // UTF-8 BOM
		return string(b[3:]), nil
	case len(b) >= 2 && b[1] == 0x00 && b[0] != 0x00: // UTF-16LE без BOM (ASCII-старт)
		return decodeUTF16(b, true), nil
	}
	return string(b), nil // UTF-8 / ANSI
}

func decodeUTF16(b []byte, le bool) string {
	u := make([]uint16, 0, len(b)/2)
	for i := 0; i+1 < len(b); i += 2 {
		if le {
			u = append(u, uint16(b[i])|uint16(b[i+1])<<8)
		} else {
			u = append(u, uint16(b[i])<<8|uint16(b[i+1]))
		}
	}
	return string(utf16.Decode(u))
}

// rdpValue возвращает значение поля key (часть после «key:тип:»). Строки .rdp
// имеют вид «имя:тип:значение», сравнение имени без учёта регистра.
func rdpValue(text, key string) (string, bool) {
	sc := bufio.NewScanner(strings.NewReader(text))
	sc.Buffer(make([]byte, 0, 64*1024), 16*1024*1024) // подпись — одна длинная строка
	kp := strings.ToLower(key) + ":"
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if strings.HasPrefix(strings.ToLower(line), kp) {
			if parts := strings.SplitN(line, ":", 3); len(parts) == 3 {
				return parts[2], true
			}
		}
	}
	return "", false
}

// isRemoteAppRdp: файл описывает сеанс RemoteApp (remoteapplicationmode:i:1).
func isRemoteAppRdp(text string) bool {
	v, ok := rdpValue(text, "remoteapplicationmode")
	return ok && strings.TrimSpace(v) == "1"
}

var oidSignedData = asn1.ObjectIdentifier{1, 2, 840, 113549, 1, 7, 2}

type pkcsContentInfo struct {
	ContentType asn1.ObjectIdentifier
	Content     asn1.RawValue `asn1:"explicit,optional,tag:0"`
}

// pkcsSignedData — SignedData ровно до signerInfos: сертификаты + первый
// SignerInfo (по его issuerAndSerial выбираем именно подписавший сертификат).
type pkcsSignedData struct {
	Version      int
	DigestAlgos  asn1.RawValue `asn1:"set"`
	ContentInfo  asn1.RawValue
	Certificates asn1.RawValue `asn1:"optional,tag:0"`
	CRLs         asn1.RawValue `asn1:"optional,tag:1"`
	SignerInfos  asn1.RawValue `asn1:"set"`
}

type issuerAndSerial struct {
	Issuer asn1.RawValue
	Serial *big.Int
}

// signerInfoHead — начало SignerInfo (version + issuerAndSerial); остаток
// (digestAlgorithm и далее) asn1.Unmarshal вернёт как rest.
type signerInfoHead struct {
	Version int
	IAS     issuerAndSerial
}

func stripSpace(s string) string {
	return strings.Map(func(r rune) rune {
		switch r {
		case ' ', '\t', '\r', '\n':
			return -1
		}
		return r
	}, s)
}

// parseCerts разбирает конкатенацию DER-сертификатов по одному, пропуская те,
// что x509 не осилил (иначе один странный сертификат в мешке скрыл бы дату).
func parseCerts(der []byte) []*x509.Certificate {
	var out []*x509.Certificate
	for len(der) > 0 {
		var raw asn1.RawValue
		rest, err := asn1.Unmarshal(der, &raw)
		if err != nil {
			break
		}
		if c, err := x509.ParseCertificate(raw.FullBytes); err == nil {
			out = append(out, c)
		}
		der = rest
	}
	return out
}

// signerCertExpiry: NotAfter именно подписавшего сертификата. Подписанта берём
// из первого SignerInfo (issuer+serial); если не удалось сопоставить — фолбэк
// на не-CA сертификат с самой ранней датой (а если все CA — самый ранний).
func signerCertExpiry(certs []*x509.Certificate, signerInfos []byte) (time.Time, bool) {
	var si signerInfoHead
	if _, err := asn1.Unmarshal(signerInfos, &si); err == nil && si.IAS.Serial != nil {
		for _, c := range certs {
			if c.SerialNumber != nil && c.SerialNumber.Cmp(si.IAS.Serial) == 0 &&
				bytes.Equal(c.RawIssuer, si.IAS.Issuer.FullBytes) {
				return c.NotAfter, true
			}
		}
	}
	var best time.Time
	found := false
	for _, c := range certs {
		if c.IsCA {
			continue
		}
		if !found || c.NotAfter.Before(best) {
			best, found = c.NotAfter, true
		}
	}
	if !found {
		for _, c := range certs {
			if !found || c.NotAfter.Before(best) {
				best, found = c.NotAfter, true
			}
		}
	}
	return best, found
}

// signatureExpiry: значение поля signature:s: — base64 с заголовком rdpsign
// (12 байт) перед PKCS#7. base64 в .rdp разбит пробелами каждые 64 символа.
// Заголовок пропускаем, находя ContentInfo по OID signedData; из PKCS#7 берём
// сертификаты и дату окончания подписавшего.
func signatureExpiry(sigB64 string) (time.Time, bool, error) {
	blob, err := base64.StdEncoding.DecodeString(stripSpace(sigB64))
	if err != nil {
		return time.Time{}, false, err
	}
	oid := []byte{0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02}
	idx := bytes.Index(blob, oid)
	if idx < 0 {
		return time.Time{}, false, errors.New("OID signedData не найден")
	}
	var ci pkcsContentInfo
	parsed := false
	for hdr := 2; hdr <= 7; hdr++ { // размер заголовка SEQUENCE перед OID (30 [8x LL...])
		start := idx - hdr
		if start < 0 {
			continue
		}
		if _, e := asn1.Unmarshal(blob[start:], &ci); e == nil && ci.ContentType.Equal(oidSignedData) {
			parsed = true
			break
		}
	}
	if !parsed {
		return time.Time{}, false, errors.New("не удалось разобрать ContentInfo")
	}
	var sd pkcsSignedData
	if _, err := asn1.Unmarshal(ci.Content.Bytes, &sd); err != nil {
		return time.Time{}, false, err
	}
	certs := parseCerts(sd.Certificates.Bytes)
	if len(certs) == 0 {
		return time.Time{}, false, errors.New("нет сертификатов в подписи")
	}
	exp, ok := signerCertExpiry(certs, sd.SignerInfos.Bytes)
	return exp, ok, nil
}

// expiryFromText: срок действия подписи по уже прочитанному тексту .rdp (файл
// читаем один раз на пути «проверили RemoteApp → показали дату»). ok=false —
// если файл не подписан или подпись не удалось разобрать (дату не показываем).
func expiryFromText(text string) (time.Time, bool) {
	sig, ok := rdpValue(text, "signature")
	if !ok || strings.TrimSpace(sig) == "" {
		return time.Time{}, false
	}
	exp, ok, err := signatureExpiry(sig)
	if err != nil {
		return time.Time{}, false
	}
	return exp, ok
}

// formatDate: дата в виде ДД.ММ.ГГГГ (в UTC, как хранится в сертификате).
func formatDate(t time.Time) string {
	u := t.UTC()
	return fmt.Sprintf("%02d.%02d.%04d", u.Day(), int(u.Month()), u.Year())
}
