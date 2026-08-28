package main

import (
	"bufio"
	"bytes"
	"crypto/x509"
	"encoding/asn1"
	"encoding/base64"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"
	"unicode/utf16"
)

// readRdpText читает .rdp с учётом кодировки: mstsc пишет UTF-16LE (BOM FF FE),
// но встречаются и UTF-8/ANSI-файлы. Ключи всегда ASCII, поэтому для ANSI
// достаточно вернуть байты как есть.
func readRdpText(path string) (string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	if len(b) >= 2 && b[0] == 0xFF && b[1] == 0xFE { // UTF-16LE
		u := make([]uint16, 0, (len(b)-2)/2)
		for i := 2; i+1 < len(b); i += 2 {
			u = append(u, uint16(b[i])|uint16(b[i+1])<<8)
		}
		return string(utf16.Decode(u)), nil
	}
	if len(b) >= 2 && b[0] == 0xFE && b[1] == 0xFF { // UTF-16BE
		u := make([]uint16, 0, (len(b)-2)/2)
		for i := 2; i+1 < len(b); i += 2 {
			u = append(u, uint16(b[i])<<8|uint16(b[i+1]))
		}
		return string(utf16.Decode(u)), nil
	}
	return string(b), nil
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

// pkcsSignedData — усечённая SignedData: нужны только сертификаты. Хвост
// (crls, signerInfos) asn1.Unmarshal вернёт как rest и не считает ошибкой.
type pkcsSignedData struct {
	Version      int
	DigestAlgos  asn1.RawValue `asn1:"set"`
	ContentInfo  asn1.RawValue
	Certificates asn1.RawValue `asn1:"optional,tag:0"`
	Rest         asn1.RawValue `asn1:"optional"`
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

// certsFromSignature: значение поля signature:s: — base64 с заголовком rdpsign
// (12 байт) перед PKCS#7. Заголовок пропускаем, находя ContentInfo по OID
// signedData; из PKCS#7 достаём вложенные сертификаты. base64 в .rdp разбит
// пробелами каждые 64 символа — их убираем.
func certsFromSignature(sigB64 string) ([]*x509.Certificate, error) {
	blob, err := base64.StdEncoding.DecodeString(stripSpace(sigB64))
	if err != nil {
		return nil, err
	}
	oid := []byte{0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02}
	idx := bytes.Index(blob, oid)
	if idx < 0 {
		return nil, errors.New("OID signedData не найден")
	}
	var ci pkcsContentInfo
	parsed := false
	for _, hdr := range []int{4, 3, 5, 2} { // длина SEQUENCE перед OID: 30 82 LL LL и т.п.
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
		return nil, errors.New("не удалось разобрать ContentInfo")
	}
	var sd pkcsSignedData
	if _, err := asn1.Unmarshal(ci.Content.Bytes, &sd); err != nil {
		return nil, err
	}
	if len(sd.Certificates.Bytes) == 0 {
		return nil, errors.New("нет сертификатов в подписи")
	}
	return x509.ParseCertificates(sd.Certificates.Bytes)
}

// leafCertExpiry: дата окончания подписывающего (не-CA) сертификата. Именно её
// увидит пользователь как «до какого числа действует файл». При нескольких
// листьях берём самый ранний; если все — CA, самый ранний из всех.
func leafCertExpiry(certs []*x509.Certificate) (time.Time, bool) {
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

// rdpSignatureExpiry: срок действия подписи .rdp-файла, если он подписан и
// подпись удалось разобрать. Иначе ok=false (дату просто не показываем).
func rdpSignatureExpiry(path string) (time.Time, bool) {
	text, err := readRdpText(path)
	if err != nil {
		return time.Time{}, false
	}
	sig, ok := rdpValue(text, "signature")
	if !ok || strings.TrimSpace(sig) == "" {
		return time.Time{}, false
	}
	certs, err := certsFromSignature(sig)
	if err != nil {
		return time.Time{}, false
	}
	return leafCertExpiry(certs)
}

// formatDate: дата в виде ДД.ММ.ГГГГ (в UTC, как хранится в сертификате).
func formatDate(t time.Time) string {
	u := t.UTC()
	return fmt.Sprintf("%02d.%02d.%04d", u.Day(), int(u.Month()), u.Year())
}
