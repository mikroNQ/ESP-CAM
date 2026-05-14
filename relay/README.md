# relay — MJPEG ретранслятор для ESP32-CAM

Маленький Go-сервис на stdlib, который держит **одно** соединение с ESP32-CAM и раздаёт видеопоток произвольному числу клиентов в локальной сети. Решает известное ограничение прошивки `CameraWebServer`: её `/stream` отдаётся одному клиенту, второй получает «занято».

## Архитектура

```
┌──────────────┐  один TCP   ┌────────────────┐   N клиентов   ┌─────────┐
│  ESP32-CAM   │────────────▶│   esp-relay    │───────────────▶│ зрители │
│  :81/stream  │  (puller)   │  (Hub fan-out) │  multipart     │ (LAN)   │
└──────────────┘             └────────────────┘                └─────────┘
```

- **puller** — одна goroutine, читает `multipart/x-mixed-replace` с камеры через `mime/multipart`, кладёт каждый JPEG в `Hub.broadcast`. При обрыве — переподключение через 2 с.
- **Hub** — потокобезопасный реестр подписчиков. Каждому клиенту выдаётся канал ёмкостью 1; при попытке отправить кадр в полный канал кадр **отбрасывается** (`select { default: }`). Это гарантирует, что ни один тормозящий зритель не блокирует источник и остальных.
- **streamHandler** — на запрос `/stream` подписывается в Hub и крутит цикл, оборачивая каждый кадр в собственные multipart-границы. `unsubscribe` вызывается через `defer`, плюс отслеживается `r.Context().Done()` — соединение чисто отпускается при отключении клиента.
- **Кэш `latest`** — последний разосланный кадр сохраняется в Hub и сразу выдаётся новому подписчику; нет «чёрного экрана» в ожидании следующего кадра. Через эндпоинт `/snapshot` доступен снаружи.

Зависимостей нет — только stdlib.

## Сборка

```powershell
cd relay
go build -ldflags="-s -w" -trimpath -o esp-relay.exe .
```

Кросс-сборка под Linux/ARM (например, для Raspberry Pi):

```powershell
$env:GOOS="linux"; $env:GOARCH="arm64"
go build -ldflags="-s -w" -trimpath -o esp-relay-linux-arm64 .
```

## Запуск

```powershell
.\esp-relay.exe -src=http://<cam-ip>:81/stream -listen=:8032
```

| Флаг      | По умолчанию                       | Описание                                       |
|-----------|------------------------------------|------------------------------------------------|
| `-src`    | `http://192.168.1.50:81/stream`    | URL MJPEG-потока ESP32-CAM (порт 81 у стоковой прошивки) |
| `-listen` | `:8080`                            | Адрес/порт, на котором релей слушает клиентов  |

## Эндпоинты

| URL          | Что отдаёт                                               |
|--------------|----------------------------------------------------------|
| `/`          | HTML-страница с `<img src="/stream">` — просмотр в браузере |
| `/stream`    | `multipart/x-mixed-replace` MJPEG для VLC, OBS, `<img>`  |
| `/snapshot`  | Текущий кадр одним JPEG — для скриптов и дашбордов        |

CORS открыт (`Access-Control-Allow-Origin: *`), картинку можно встраивать с любой страницы в LAN.

## Открыть фаервол Windows

```powershell
New-NetFirewallRule -DisplayName "ESP relay 8032" -Direction Inbound `
                    -Protocol TCP -LocalPort 8032 -Action Allow
```

## Автозапуск 24/7 — Task Scheduler

```powershell
$action  = New-ScheduledTaskAction -Execute "C:\путь\esp-relay.exe" `
                                   -Argument "-src=http://<cam-ip>:81/stream -listen=:8032"
$trigger = New-ScheduledTaskTrigger -AtStartup
$princ   = New-ScheduledTaskPrincipal -UserId "SYSTEM" -RunLevel Highest
$set     = New-ScheduledTaskSettingsSet -RestartCount 999 `
                                        -RestartInterval (New-TimeSpan -Minutes 1) `
                                        -StartWhenAvailable
Register-ScheduledTask -TaskName "ESP-CAM Relay" -Action $action `
                       -Trigger $trigger -Principal $princ -Settings $set
```

Стартует под `SYSTEM` при загрузке Windows, перезапускается раз в минуту при падении.

## Ограничения

- Источник *должен* отдавать `multipart/x-mixed-replace` (стандартный MJPEG ESP32-CAM подходит). RTSP не поддерживается.
- Все клиенты получают кадры с FPS источника или ниже (если канал клиента не успевает — кадры дропаются).
- Нет авторизации. Если релей публикуется наружу из LAN — закрыть reverse-proxy с basic-auth или TLS-терминацией.
