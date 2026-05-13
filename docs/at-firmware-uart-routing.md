# Stock Espressif AT v1.1.2 на ESP32-CAM: почему AT не отвечает на U0R/U0T

## TL;DR

Если на модуле залита **stock-прошивка Espressif AT v1.1.2** и ты не получаешь ответов от `AT`-команд на UART0 (`U0R`/`U0T`) — это не баг, не битая прошивка и не дохлый провод. Это **дефолтная архитектура UART** в Espressif AT для ESP32:

- `UART0` (GPIO1=U0T / GPIO3=U0R) — **только системный лог ESP-IDF** (бутлог, `ESP_LOGI/W/E`)
- `UART1` (GPIO16 RX / GPIO17 TX) — **AT-команды и ответы**

На клонах AI-Thinker `GPIO17` физически не выведен на гребёнки — занят линией PSRAM. Поэтому **двусторонняя AT-связь через стандартное подключение к U0R/U0T невозможна**. Прочитать ответ просто негде.

Решение в этом проекте — не использовать AT firmware вообще, а прошить родной `CameraWebServer` под Arduino, где `Serial` сидит на UART0 (наши же провода), а конфигурация WiFi идёт через captive portal без интерактивных команд от хоста.

## Как это выглядит на проводах

При запуске модуля с AT v1.1.2 на UART0 (`U0T → RXD CH340`) ты увидишь стандартный бутлог:

```
ets Jul 29 2019 12:21:46
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
...
I (29) boot: ESP-IDF v3.0.3 2nd stage bootloader
...
Bin version(Wroom32):1.1.2
I (652) wifi: wifi firmware version: de47fad
...
I (721) wifi: mode : softAP (28:05:a5:24:93:c9)
I (729) wifi: mode : sta (28:05:a5:24:93:c8) + softAP (28:05:a5:24:93:c9)
I (733) wifi: mode : softAP (28:05:a5:24:93:c9)
```

Бутлог идёт, WiFi инициализируется, но **обычного для AT-firmware приглашения `ready\r\n` нет**. Это первый признак: `ready` печатается в AT-канал (UART1/GPIO17), а ты слушаешь UART0.

Если в этот момент послать `AT\r\n` на TXD адаптера → U0R ESP — никакого ответа. И не будет, сколько бы baud-rate ты ни перебирал. Парсер AT на UART0 не слушает.

## Чем доказывается, что AT действительно сидит на UART1

Эксперимент:

1. Оставляем `белый ← U0T` (читаем лог) как есть.
2. **Перевешиваем зелёный** с `U0R` на пин **`IO16`** (он есть на правой гребёнке клона).
3. Шлём `AT+RST\r\n` через CH340 TXD → IO16.

В логах на UART0 появляется:

```
I (177652) wifi: flush txq
I (177653) wifi: stop sw txq
I (177654) wifi: lmac stop hw txq

ets Jul 29 2019 12:21:46
rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
...
```

Ключевые два индикатора:

- `wifi: flush txq` / `stop sw txq` — корректное завершение WiFi-стека перед перезагрузкой. Так делает только программный reset через AT, а не передёргивание питания.
- `rst:0xc (SW_CPU_RESET)` — программно вызванный reset CPU. При power-cycle здесь было бы `rst:0x1 (POWERON_RESET)`.

Это окончательно подтверждает: байты с TXD CH340, попавшие на GPIO16, **дошли до AT-парсера**, он распознал `AT+RST` и выполнил soft-reset. AT действительно слушает UART1 RX = GPIO16.

В обратную сторону (`AT TX = GPIO17`) уже не подключиться — пина нет.

## Какие AT-firmware ведут себя так

Все официальные релизы Espressif **ESP32-AT v1.x** для классического ESP32 (WROOM-32, WROVER) идут с дефолтом `UART0=log, UART1=AT`. Это явно прописано в их `README`/`docs/AT_Binary_Lists`. Конкретные сигнатуры в бутлоге:

- `Bin version(Wroom32):1.1.x` или `Bin version(WroomB):1.1.x`
- `ESP-IDF v3.0.x`
- наличие партиции `at_customize` в таблице (виден в бутлоге как `3 at_customize  unknown  40 00 00020000 000e0000`)

Начиная с **ESP-AT v2.x** UART для AT стал перенастраиваемым на любые пины через команду `AT+UART_DEF` (если у тебя уже есть к нему доступ хоть как-то) или через `at_customize.bin` (sdkconfig partition table override). На 1.x перепиновка делается только пересборкой прошивки.

## Что можно сделать, если ты упёрся в это

В порядке возрастания усилий:

1. **Прошить заново** на не-AT firmware (как этот репо) — самый быстрый путь, если AT не требуется.
2. **Прошить ESP-AT v2.x** — там UART2 default, RX/TX можно ремапнуть на свободные пины ESP-CAM (`IO12-IO15` если SD не используется) через `AT+UART_DEF`.
3. **Найти GPIO17** на нестандартных вариантах плат — некоторые «не-AI-Thinker» клоны ESP32-CAM выводят его на отдельный test pad. На стандартном AI-Thinker — забудь.
4. **Жить с однонаправленной связью**: команды на IO16 шлёшь, эффект видишь в логах на U0T. Подходит для разовой настройки (`AT+CWJAP_DEF` сохраняет SSID/пасс в NVS — потом модуль сам коннектится). Не подходит для нормальной работы.

## Источники

- Espressif ESP-AT documentation: https://docs.espressif.com/projects/esp-at/en/latest/
- AT v1.1 release: https://github.com/espressif/esp-at/releases (исторические бинари там же)
- AI-Thinker ESP32-CAM pinout: внутренний даташит AI-Thinker (PDF гуляет по форумам); официальной публичной документации с указанием wiring PSRAM↔GPIO17 нет, но это подтверждается через `idf.py menuconfig` дефолты для модуля и фактическим поведением (попытки использовать GPIO17 как UART ломают PSRAM-операции).
