package main

import (
	"crypto/x509"
	"fmt"
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Персональный клиентский сертификат для удалёнки живёт в хранилище
// CurrentUser\My и выдан нашими УЦ (ufa-ca01/ufa-ca02). За personalCertWarnDays
// дней до конца (и после) — предупреждаем при каждом подключении.
const personalCertWarnDays = 30

var (
	crypt32 = windows.NewLazySystemDLL("crypt32.dll")

	procCertOpenSystemStoreW        = crypt32.NewProc("CertOpenSystemStoreW")
	procCertEnumCertificatesInStore = crypt32.NewProc("CertEnumCertificatesInStore")
	procCertCloseStore              = crypt32.NewProc("CertCloseStore")
)

// certContext повторяет CERT_CONTEXT (нужны только pbCertEncoded/cbCertEncoded).
type certContext struct {
	encodingType uint32
	encoded      *byte
	encodedLen   uint32
	certInfo     uintptr
	store        uintptr
}

func issuerIsOurs(iss string) bool {
	iss = strings.ToLower(iss)
	return strings.Contains(iss, "ufa-ca01") || strings.Contains(iss, "ufa-ca02")
}

// personalCertExpiry: ближайшая дата окончания и имя владельца среди личных
// сертификатов (CurrentUser\My), выданных нашими УЦ. ok=false — таких нет
// (или хранилище недоступно) — тогда предупреждать не о чем.
func personalCertExpiry() (exp time.Time, name string, ok bool) {
	storeName, err := windows.UTF16PtrFromString("MY")
	if err != nil {
		return time.Time{}, "", false
	}
	hStore, _, _ := procCertOpenSystemStoreW.Call(0, uintptr(unsafe.Pointer(storeName)))
	if hStore == 0 {
		return time.Time{}, "", false
	}
	defer procCertCloseStore.Call(hStore, 0)

	var ctx uintptr
	for {
		// Передаём предыдущий контекст — API освобождает его и возвращает следующий;
		// на NULL перечисление заканчивается (последний контекст тоже освобождён).
		ctx, _, _ = procCertEnumCertificatesInStore.Call(hStore, ctx)
		if ctx == 0 {
			break
		}
		cc := (*certContext)(unsafe.Pointer(ctx))
		if cc.encoded == nil || cc.encodedLen == 0 {
			continue
		}
		der := make([]byte, cc.encodedLen) // копия: память принадлежит хранилищу
		copy(der, unsafe.Slice(cc.encoded, cc.encodedLen))
		cert, e := x509.ParseCertificate(der)
		if e != nil || !issuerIsOurs(cert.Issuer.String()) {
			continue
		}
		if !ok || cert.NotAfter.Before(exp) {
			exp, name, ok = cert.NotAfter, cert.Subject.CommonName, true
		}
	}
	return exp, name, ok
}

// pluralDays: правильное склонение «день/дня/дней».
func pluralDays(n int) string {
	if n < 0 {
		n = -n
	}
	if n%100 >= 11 && n%100 <= 14 {
		return "дней"
	}
	switch n % 10 {
	case 1:
		return "день"
	case 2, 3, 4:
		return "дня"
	}
	return "дней"
}

// warnIfPersonalCertExpiring: при подключении, если персональный сертификат
// истекает (≤ personalCertWarnDays) или уже истёк — показываем понятное окно
// ошибки с обратным отсчётом (нельзя закрыть первые 5 секунд).
func warnIfPersonalCertExpiring() {
	exp, name, ok := personalCertExpiry()
	if !ok {
		return
	}
	left := time.Until(exp)
	days := int(left / (24 * time.Hour))
	if days > personalCertWarnDays {
		return
	}
	if name == "" {
		name = "—"
	}

	showLockedError("Срок действия сертификата удалённого доступа",
		certWarningBody(exp, name, days, left <= 0))
}

// certWarningBody: текст окна предупреждения. Отдельно для «скоро истечёт» и
// «уже истёк» (у истёкшего нет строки «Осталось» и фразы про «после этой даты»).
func certWarningBody(exp time.Time, name string, days int, expired bool) string {
	if expired {
		return fmt.Sprintf(
			"Ваш персональный сертификат для удалённого доступа истёк.\n\n"+
				"Владелец: %s\n"+
				"Срок действия: %s\n\n"+
				"Для продолжения использования удалённого доступа оформите "+
				"заявку в Service Desk.",
			name, formatDate(exp))
	}
	return fmt.Sprintf(
		"Ваш персональный сертификат для удалённого доступа скоро истечёт.\n\n"+
			"Владелец: %s\n"+
			"Срок действия: %s\n"+
			"Осталось: %d %s\n\n"+
			"После этой даты удалённый доступ будет невозможен.\n"+
			"Для продолжения использования удалённого доступа оформите "+
			"заявку в Service Desk.",
		name, formatDate(exp), days, pluralDays(days))
}
