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

```bash
cd relay
go build -ldflags="-s -w" -trimpath -o esp-relay .
```

> Windows: добавь `.exe` к имени (`-o esp-relay.exe`).

Кросс-сборка под Linux/ARM (например, для Raspberry Pi):

```bash
GOOS=linux GOARCH=arm64 go build -ldflags="-s -w" -trimpath -o esp-relay-linux-arm64 .
```

## Запуск

```bash
./esp-relay -src=http://<cam-ip>:81/stream -listen=:8032
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

## Фаервол (macOS)

macOS по умолчанию не блокирует входящие подключения для CLI-бинарей. Если
включён Application Firewall (Системные настройки → Сеть → Файрвол), при первом
запуске появится диалог «Разрешить входящие подключения для esp-relay» — нажми
**Разрешить**. Программно:

```bash
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$(pwd)/esp-relay"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$(pwd)/esp-relay"
```

> Windows: `New-NetFirewallRule -DisplayName "ESP relay 8032" -Direction Inbound -Protocol TCP -LocalPort 8032 -Action Allow`

## Автозапуск 24/7 — launchd (macOS)

Создай `~/Library/LaunchAgents/com.espcam.relay.plist` (подставь свои путь к
бинарю и `<cam-ip>`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>            <string>com.espcam.relay</string>
  <key>ProgramArguments</key>
  <array>
    <string>/Users/ВАШ/ESP-CAM/relay/esp-relay</string>
    <string>-src=http://<cam-ip>:81/stream</string>
    <string>-listen=:8032</string>
  </array>
  <key>RunAtLoad</key>        <true/>
  <key>KeepAlive</key>        <true/>   <!-- перезапуск при падении -->
  <key>StandardErrorPath</key><string>/tmp/esp-relay.err.log</string>
  <key>StandardOutPath</key>  <string>/tmp/esp-relay.out.log</string>
</dict>
</plist>
```

Загрузить / выгрузить:

```bash
launchctl load   ~/Library/LaunchAgents/com.espcam.relay.plist   # старт + автозапуск при логине
launchctl unload ~/Library/LaunchAgents/com.espcam.relay.plist   # остановить и убрать
```

Стартует при входе пользователя и перезапускается при падении (`KeepAlive`).

> Windows-вариант (Task Scheduler под `SYSTEM`) — см. историю репозитория.

## Ограничения

- Источник *должен* отдавать `multipart/x-mixed-replace` (стандартный MJPEG ESP32-CAM подходит). RTSP не поддерживается.
- Все клиенты получают кадры с FPS источника или ниже (если канал клиента не успевает — кадры дропаются).
- Нет авторизации. Если релей публикуется наружу из LAN — закрыть reverse-proxy с basic-auth или TLS-терминацией.
