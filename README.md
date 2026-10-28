# rdpkey

Утилита проброса горячих клавиш в RDP-сеанс RemoteApp работодателя. Перехватывает системные аккорды, которые иначе съедает локальная Windows (Win, Win+E/D/R/V/Z/Tab, Ctrl+Esc, Alt+Tab, Alt+Shift+Tab, PrintScreen, Win+Shift+S), и пробрасывает их в удалённое приложение.

## Установка

Скачать из [релизов](https://github.com/timbrs/rdp-app/releases): `rdpkey.exe` (standalone) или `rdpkey-<версия>.7z` (exe + пример `config.ini` + справка).

## Использование

- GUI: двойной клик по `rdpkey.exe`.
- Запуск удалёнки с пробросом: `rdpkey.exe <файл>.rdp` либо кнопка в GUI.
- Хоткеи включаются чекбоксами в окне; конфиг — `%LOCALAPPDATA%\rdpkey\config.ini`.

## Безопасность и антивирус

Бинарь без установщика и без обращений к сторонним серверам. Из-за небольшого нестандартного PE-файла ML-эвристика Microsoft Defender иногда даёт **ложное срабатывание** `Trojan:Win32/Wacatac.C!ml`. Такие детекты отправляются в Microsoft и признаются чистыми («Not malware»).

Релиз v1.0.12 проверен на VirusTotal (0 срабатываний):

- SHA-256: `a84be4bc995930362a96894f1c27bc84a2e86a15a8db397e23ef932180e60a76`
- Отчёт: https://www.virustotal.com/gui/file/a84be4bc995930362a96894f1c27bc84a2e86a15a8db397e23ef932180e60a76
