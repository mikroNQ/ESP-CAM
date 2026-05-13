# ESP32-CAM HTTP-стрим с WiFiManager

Прошивка для AI-Thinker ESP32-CAM (или клона на WROOM-32 + OV2640/OV3660). Сборка на базе примера `CameraWebServer` из ESP32 Arduino core 3.3.8 с заменой хардкоженных WiFi-credentials на [tzapu/WiFiManager](https://github.com/tzapu/WiFiManager) — при первом старте поднимается captive portal, сетку выбираешь через браузер.

## Что собиралось

- **Плата**: AI-Thinker ESP32-CAM (клон, WROOM-32 без PSRAM в нашем экземпляре, сенсор OV3660)
- **USB-TTL**: HW-597 (CH340G) — 6-пиновый: `5V | VCC | 3V3 | TXD | RXD | GND`
- **ESP32 Arduino core**: `esp32:esp32@3.3.8`
- **Библиотеки**: `WiFiManager@2.0.17`
- **FQBN**: `esp32:esp32:esp32cam`

## Распиновка для прошивки (UART0 + boot-strap)

| Пин USB-TTL | Пин ESP32-CAM | Назначение |
|-------------|---------------|------------|
| 5V          | 5V            | Питание модуля |
| GND         | GND           | Земля |
| TXD         | U0R (GPIO3)   | Команды/прошивка от хоста |
| RXD         | U0T (GPIO1)   | Лог/ответы от модуля |
| `VCC ↔ 3V3` *на самом адаптере* | — | Селектор уровня TX/RX = 3.3V (ESP-safe) |
| **`IO0 ↔ GND`** (только на время прошивки) | — | Вход в download mode |

После заливки **снять перемычку IO0↔GND** и передёрнуть 5V — модуль уйдёт в обычный flash boot.

![Подключение к UART-стороне](docs/images/wiring-uart-side.jpg)
![Подключение к стороне питания](docs/images/wiring-power-side.jpg)

## Сборка и прошивка через arduino-cli

```powershell
# Однократно
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.8
arduino-cli lib install "WiFiManager"

# Каждый раз
arduino-cli compile --fqbn esp32:esp32:esp32cam CameraWebServer
arduino-cli upload  --fqbn esp32:esp32:esp32cam --port COM30 CameraWebServer
```

Подставь свой COM-порт. На Windows номер можно посмотреть в Диспетчере устройств → Порты (COM и LPT), либо через PowerShell:

```powershell
Get-PnpDevice -Class Ports -PresentOnly | Where-Object FriendlyName -match 'CH340|FTDI|CP210|Prolific'
```

## Первый запуск

1. После прошивки сними перемычку IO0↔GND и передёрни питание.
2. С телефона/ноута найди WiFi-сеть **`ESP32-CAM-Setup`** (без пароля), подключись.
3. Должен открыться captive portal автоматически. Если нет — открой `http://192.168.4.1`.
4. **Configure WiFi** → выбери свою сеть из списка, введи пароль → Save.
5. ESP-CAM сохранит данные в NVS, перезагрузится и подцепится к домашней сети.
6. В Serial Monitor (115200) появится строка вида `Camera Ready! Use 'http://x.x.x.x' to connect`.
7. Открой этот IP в браузере — увидишь интерфейс с потоком и настройками камеры.

## Особенности диагностики (на что напоролись)

- **Stock AT-прошивка** Espressif AT v1.1.2, которая может быть на модуле «из коробки», использует **UART1** для AT-команд (`RX=GPIO16`, `TX=GPIO17`), а **UART0** только для системного лога. На AI-Thinker ESP-CAM GPIO17 не выведен на гребёнки (занят PSRAM-линией), поэтому двусторонняя AT-связь через стандартное подключение к U0R/U0T невозможна. Это объясняет, почему по дефолту бутлог читается, а ответ на `AT` не приходит. Лечится перепрошивкой на родной CameraWebServer (этот репо) или на ESP-AT v2.x, где UART для AT перенастраиваемый. Полный разбор с экспериментальным подтверждением через `AT+RST → SW_CPU_RESET` — в [docs/at-firmware-uart-routing.md](docs/at-firmware-uart-routing.md).
- **HW-597 CH340-модуль**: пин `VCC` — селектор уровня TX/RX, а не питание. Если оставить его в воздухе, драйвер UART не питается и связь не работает ни в одном направлении. Замкнуть джампером/проводком `VCC ↔ 3V3` на самой плате.
- **PL2303 кабели**: клоны блокируются драйвером macOS Big Sur+ (Apple переключилась только на оригинальные Prolific). На Windows встроенный CDC-драйвер всеяден и тот же клон работает.
- **«Hard resetting via RTS pin»** от esptool **физически ничего не делает**, если RTS не разведён до EN на адаптере (на бюджетных кабелях обычно так). После прошивки приходится руками снимать IO0↔GND и передёргивать питание.

## Структура

```
CameraWebServer/
├── CameraWebServer.ino    # main: camera init + WiFiManager + сервер
├── app_httpd.cpp          # HTTP-сервер, JPEG-стрим, REST для настроек
├── board_config.h         # выбор модели — здесь CAMERA_MODEL_AI_THINKER
├── camera_pins.h          # GPIO-маппинг для всех поддерживаемых плат
├── camera_index.h         # gzip'нутый HTML интерфейса
└── partitions.csv         # 3MB app + OTA, нужно для >2MB прошивки
```
