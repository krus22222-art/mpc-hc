# Спільний перегляд (mpc-share)

Модуль трансляції для MPC-HC series-player: передає **лише область відео** плеєра і **лише звук плеєра**
у браузер іншої людини (планшет, телефон, ПК) через WebRTC. Глядачу нічого встановлювати не потрібно.

Запуск: меню плеєра **Файл → Спільний перегляд…** (або `mpc-share.exe` поруч із `mpc-hc64.exe`).

## Як це працює

| Частина | Реалізація |
|---|---|
| Відео | Windows Graphics Capture вікна плеєра → обрізка до області відео → libyuv (масштаб, NV12) → кодер H.264 Media Foundation (Windows, Constrained Baseline, CBR 4.5 Мбіт/с, до 1080p30) |
| Звук | WASAPI process loopback (лише процес MPC-HC, Windows 10 2004+) → Opus 128 кбіт/с |
| Передача | libdatachannel (WebRTC: ICE, DTLS-SRTP, RTP; NACK, PLI, RTCP SR) |
| Сигналізація | етап 1: вбудований HTTP-сервер лише на `127.0.0.1` (сторінка глядача + обмін SDP). Етап 2: ретрансляція через інтернет (у розробці) |

Окремий процес: збій трансляції не впливає на відтворення.

## Особливості сумісності

* **Mbed TLS замість OpenSSL**: збірка OpenSSL потребує Perl, якого блокує Windows Smart App Control.
* **Модуль — DTLS-клієнт** (`a=setup:active` в оферті): сучасні браузери надсилають ClientHello,
  більший за одну датаграму (постквантовий обмін ключами), а Mbed TLS як сервер не збирає
  фрагментований ClientHello.
* **TLS 1.3 у Mbed TLS вимкнено**: WebRTC використовує DTLS 1.2.
* `psa_crypto_init()` викликається один раз при старті (вимога Mbed TLS 3.6).

## Збірка

```powershell
powershell -ExecutionPolicy Bypass -File src\SeriesShare\build-share.ps1
```

Потрібні Visual Studio 2022 Build Tools (C++) і vcpkg. Скрипт ставить залежності (з портами з
`vcpkg-overlay/`), збирає `mpc-share.exe` і копіює його в `bin\mpc-hc_x64`.

## Ліцензії залежностей

libdatachannel, libjuice, usrsctp — MPL-2.0 / BSD; libSRTP — BSD-3-Clause; Mbed TLS — Apache-2.0;
Opus — BSD-3-Clause; libyuv — BSD-3-Clause; nlohmann/json — MIT; plog — MIT.
