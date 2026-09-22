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

Релиз v1.0.11 проверен на VirusTotal:

- SHA-256: `d36327b22dcbe65d71a8e371e0d5471687be8d267991074f3974447f3fffc9af`
- Отчёт: https://www.virustotal.com/gui/file/d36327b22dcbe65d71a8e371e0d5471687be8d267991074f3974447f3fffc9af
