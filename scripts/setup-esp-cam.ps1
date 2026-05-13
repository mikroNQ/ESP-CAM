<#
.SYNOPSIS
    Интерактивная прошивка ESP32-CAM (AI-Thinker) CameraWebServer + WiFiManager.

.DESCRIPTION
    Проводит по всей процедуре с нуля:
      1. Проверяет/ставит arduino-cli, esp32 core 3.3.8, WiFiManager 2.0.17
      2. Находит USB-TTL переходник в системе и предлагает выбрать
      3. (Опц.) Loopback-тест для проверки адаптера и проводов
      4. Показывает схему подключения, ждёт подтверждения
      5. Открывает serial monitor для бутлога — проверяет, что ESP жив
      6. Просит поставить IO0↔GND и передёрнуть питание для входа в download mode
      7. Компилирует и заливает CameraWebServer.ino
      8. Просит снять IO0↔GND, передёрнуть питание
      9. Открывает monitor для отлова IP от WiFiManager, выводит ссылку

.PARAMETER ComPort
    Указать COM-порт явно (иначе скрипт автодетектит/спрашивает).

.PARAMETER SketchDir
    Путь к папке со sketch (default: ..\CameraWebServer от скрипта).

.PARAMETER SkipLoopback
    Пропустить loopback-тест.

.PARAMETER SkipBootCheck
    Пропустить проверку бутлога перед прошивкой.

.PARAMETER NoBrowser
    Не открывать браузер после успешной прошивки.

.EXAMPLE
    .\setup-esp-cam.ps1

.EXAMPLE
    .\setup-esp-cam.ps1 -ComPort COM5 -SkipLoopback
#>

[CmdletBinding()]
param(
    [string]$ComPort,
    [string]$SketchDir,
    [switch]$SkipLoopback,
    [switch]$SkipBootCheck,
    [switch]$NoBrowser
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# ============================================================
# Константы
# ============================================================
$REPO_ROOT       = Split-Path -Parent $PSScriptRoot
$DEFAULT_SKETCH  = Join-Path $REPO_ROOT 'CameraWebServer'
$TOOLS_DIR       = Join-Path $REPO_ROOT 'tools'
$ARDUINO_CLI     = Join-Path $TOOLS_DIR 'arduino-cli.exe'
$ARDUINO_CLI_URL = 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip'
$ESP32_INDEX_URL = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
$ESP32_CORE_VER  = '3.3.8'
$WM_LIB_VER      = '2.0.17'
$BOARD_FQBN      = 'esp32:esp32:esp32cam'
$BAUD            = 115200

# ============================================================
# UI-хелперы
# ============================================================
function Write-Step  ([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok    ([string]$msg) { Write-Host "[OK]  $msg" -ForegroundColor Green }
function Write-Warn2 ([string]$msg) { Write-Host "[!!]  $msg" -ForegroundColor Yellow }
function Write-Err2  ([string]$msg) { Write-Host "[ERR] $msg" -ForegroundColor Red }
function Write-Hint  ([string]$msg) { Write-Host "      $msg" -ForegroundColor DarkGray }

function Read-Confirm ([string]$prompt, [bool]$default = $true) {
    $hint = if ($default) { '[Y/n]' } else { '[y/N]' }
    while ($true) {
        $ans = Read-Host "$prompt $hint"
        if ([string]::IsNullOrWhiteSpace($ans)) { return $default }
        switch ($ans.Trim().ToLower()) {
            'y' { return $true }
            'yes' { return $true }
            'д' { return $true }
            'да' { return $true }
            'n' { return $false }
            'no' { return $false }
            'н' { return $false }
            'нет' { return $false }
        }
    }
}

function Pause-Enter ([string]$msg = 'нажми Enter когда готов') {
    Write-Host ""
    Write-Host "    $msg ..." -ForegroundColor DarkYellow -NoNewline
    [void](Read-Host)
}

# ============================================================
# Серийный порт — высокоуровневые операции
# ============================================================
function Open-SerialPort ([string]$name, [int]$baud = 115200) {
    $port = New-Object System.IO.Ports.SerialPort $name, $baud, 'None', 8, 'One'
    $port.ReadTimeout  = 200
    $port.WriteTimeout = 200
    $port.DtrEnable    = $false
    $port.RtsEnable    = $false
    $port.Open()
    Start-Sleep -Milliseconds 100
    $port.DiscardInBuffer()
    return $port
}

function Read-Serial ([System.IO.Ports.SerialPort]$port, [int]$ms) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    $buf = New-Object System.Collections.Generic.List[byte]
    while ((Get-Date) -lt $deadline) {
        if ($port.BytesToRead -gt 0) {
            $chunk = New-Object byte[] $port.BytesToRead
            $n = $port.Read($chunk, 0, $chunk.Length)
            for ($i = 0; $i -lt $n; $i++) { $buf.Add($chunk[$i]) }
        } else {
            Start-Sleep -Milliseconds 30
        }
    }
    return ,$buf.ToArray()
}

function Format-SerialAscii ([byte[]]$bytes) {
    return -join ($bytes | ForEach-Object {
        if ($_ -ge 0x20 -and $_ -lt 0x7F) { [char]$_ }
        elseif ($_ -eq 0x0A) { "`n" }
        elseif ($_ -eq 0x0D) { '' }
        else { '.' }
    })
}

# ============================================================
# Инструменты — детект и установка
# ============================================================
function Resolve-ArduinoCli {
    if (Test-Path $ARDUINO_CLI) { return $ARDUINO_CLI }
    $existing = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($existing) { return $existing.Source }
    return $null
}

function Install-ArduinoCli {
    Write-Step "Скачиваю arduino-cli в $TOOLS_DIR"
    if (-not (Test-Path $TOOLS_DIR)) { New-Item -ItemType Directory -Path $TOOLS_DIR | Out-Null }
    $zip = Join-Path $TOOLS_DIR 'arduino-cli.zip'
    Invoke-WebRequest -Uri $ARDUINO_CLI_URL -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $TOOLS_DIR -Force
    Remove-Item $zip
    if (-not (Test-Path $ARDUINO_CLI)) { throw "arduino-cli.exe не появился после распаковки" }
    Write-Ok "arduino-cli готов: $ARDUINO_CLI"
}

function Ensure-ArduinoCli {
    Write-Step "Проверяю arduino-cli"
    $cli = Resolve-ArduinoCli
    if (-not $cli) {
        Install-ArduinoCli
        $cli = $ARDUINO_CLI
    }
    $ver = & $cli version 2>&1 | Select-Object -First 1
    Write-Ok "arduino-cli: $ver"
    return $cli
}

function Ensure-Esp32Core ([string]$cli) {
    Write-Step "Проверяю ESP32 Arduino core $ESP32_CORE_VER"
    $installed = & $cli core list 2>&1 | Select-String -Pattern "^esp32:esp32\s+$([regex]::Escape($ESP32_CORE_VER))"
    if ($installed) {
        Write-Ok "esp32:esp32@$ESP32_CORE_VER уже установлен"
        return
    }
    # Add URL if missing
    $urls = (& $cli config get board_manager.additional_urls 2>&1) -join ' '
    if ($urls -notlike "*$ESP32_INDEX_URL*") {
        Write-Hint "Добавляю Espressif URL в board_manager.additional_urls"
        & $cli config init --overwrite 2>&1 | Out-Null
        & $cli config add board_manager.additional_urls $ESP32_INDEX_URL 2>&1 | Out-Null
    }
    Write-Hint "Обновляю индекс boards..."
    & $cli core update-index 2>&1 | Out-Null
    Write-Hint "Ставлю esp32:esp32@$ESP32_CORE_VER (~250МБ, 2-5 мин)..."
    & $cli core install "esp32:esp32@$ESP32_CORE_VER" 2>&1 | Select-Object -Last 5 | ForEach-Object { Write-Hint $_ }
    Write-Ok "esp32 core установлен"
}

function Ensure-WiFiManager ([string]$cli) {
    Write-Step "Проверяю библиотеку WiFiManager $WM_LIB_VER"
    $installed = & $cli lib list 2>&1 | Select-String -Pattern "^WiFiManager\s+$([regex]::Escape($WM_LIB_VER))"
    if ($installed) {
        Write-Ok "WiFiManager@$WM_LIB_VER уже стоит"
        return
    }
    Write-Hint "Ставлю WiFiManager@$WM_LIB_VER..."
    & $cli lib install "WiFiManager@$WM_LIB_VER" 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Hint $_ }
    Write-Ok "WiFiManager установлен"
}

# ============================================================
# Детект COM-порта
# ============================================================
function Get-SerialAdapters {
    return Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object {
        $name = $_.FriendlyName
        $com  = if ($name -match '\(COM(\d+)\)') { "COM$($Matches[1])" } else { $null }
        if (-not $com) { return }
        $chip = 'unknown'
        if ($name -match 'CH340|CH341') { $chip = 'CH340' }
        elseif ($name -match 'CP210') { $chip = 'CP2102' }
        elseif ($name -match 'FTDI|FT232|USB Serial Port') { $chip = 'FTDI' }
        elseif ($name -match 'Prolific|PL2303') { $chip = 'PL2303 (на macOS капризен)' }
        elseif ($name -match 'Silicon Labs|CP21') { $chip = 'CP2102' }
        [PSCustomObject]@{
            Com  = $com
            Chip = $chip
            Name = $name
        }
    } | Sort-Object Com
}

function Select-ComPort ([string]$override) {
    Write-Step "Поиск USB-TTL переходника"
    if ($override) {
        Write-Hint "Использую указанный: $override"
        return $override
    }
    $adapters = Get-SerialAdapters
    if (-not $adapters -or $adapters.Count -eq 0) {
        Write-Err2 "Ни одного COM-порта не найдено. Воткни USB-TTL и нажми Enter."
        Pause-Enter
        $adapters = Get-SerialAdapters
        if (-not $adapters) { throw "Так и не нашёл COM-порт" }
    }
    if ($adapters.Count -eq 1) {
        $a = $adapters[0]
        Write-Ok "Найден один: $($a.Com) ($($a.Chip)) — $($a.Name)"
        if (Read-Confirm "Использовать $($a.Com)?") { return $a.Com }
        throw "Отменено пользователем"
    }
    Write-Host ""
    Write-Host "  Найдено несколько портов:" -ForegroundColor White
    for ($i = 0; $i -lt $adapters.Count; $i++) {
        Write-Host ("    [{0}] {1,-6} {2,-10} {3}" -f ($i+1), $adapters[$i].Com, $adapters[$i].Chip, $adapters[$i].Name)
    }
    while ($true) {
        $idx = Read-Host "  Какой? (номер)"
        if ($idx -match '^\d+$' -and [int]$idx -ge 1 -and [int]$idx -le $adapters.Count) {
            return $adapters[[int]$idx - 1].Com
        }
        Write-Warn2 "Введи число от 1 до $($adapters.Count)"
    }
}

# ============================================================
# Loopback test
# ============================================================
function Test-Loopback ([string]$port) {
    Write-Step "Loopback-тест на $port"
    Write-Host "  Замкни TX↔RX на переходнике (одной коротенькой проволочкой или пинцетом)" -ForegroundColor White
    Write-Hint "Это проверит и сам адаптер, и его pin-headers"
    Pause-Enter "TX↔RX замкнуты, нажми Enter"

    $bauds = @(9600, 115200, 230400)
    $payload = [byte[]](0x55, 0xAA, 0x01, 0x02, 0x7E, 0x80, 0xFF)
    $allOk = $true
    foreach ($baud in $bauds) {
        $p = $null
        try {
            $p = Open-SerialPort $port $baud
            $p.Write($payload, 0, $payload.Length); $p.BaseStream.Flush()
            $rx = Read-Serial $p 500
            $eq = ($rx.Length -eq $payload.Length)
            if ($eq) {
                for ($i=0; $i -lt $payload.Length; $i++) {
                    if ($rx[$i] -ne $payload[$i]) { $eq = $false; break }
                }
            }
            if ($eq) {
                Write-Ok ("$baud baud: эхо OK ({0} байт)" -f $rx.Length)
            } else {
                Write-Err2 ("$baud baud: эхо ИСКАЖЕНО или НЕТ ({0}/{1} байт)" -f $rx.Length, $payload.Length)
                $allOk = $false
            }
        } catch {
            Write-Err2 "$baud baud: $($_.Exception.Message)"
            $allOk = $false
        } finally {
            if ($p -and $p.IsOpen) { $p.Close() }
        }
    }
    if (-not $allOk) {
        Write-Warn2 "Адаптер не прошёл loopback на всех скоростях — проверь штырьки/перемычку, замени переходник"
        if (-not (Read-Confirm "Продолжить несмотря на это?" $false)) { throw "Отменено: дохлый адаптер" }
    }
    Write-Step "Сними перемычку TX↔RX перед подключением к ESP-CAM"
    Pause-Enter "Перемычка снята, нажми Enter"
}

# ============================================================
# Шаги пайплайна
# ============================================================
function Show-WiringGuide {
    Write-Step "Распиновка USB-TTL ↔ ESP32-CAM (для прошивки и работы)"
    @"
    ┌────────────────┐         ┌──────────────────┐
    │ USB-TTL adapter│         │   ESP32-CAM      │
    ├────────────────┤         ├──────────────────┤
    │ 5V             │ ──────▶ │ 5V               │
    │ GND            │ ──────▶ │ GND              │
    │ TXD            │ ──────▶ │ U0R  (GPIO3, RX) │
    │ RXD            │ ◀────── │ U0T  (GPIO1, TX) │
    └────────────────┘         └──────────────────┘

    Если на адаптере есть отдельный пин VCC (как на HW-597) — замкни VCC↔3V3
    на самом адаптере (короткой перемычкой или джампером). Это включает
    3.3V логику на TXD/RXD, иначе ESP-CAM не примет сигнал.

    Если на адаптере есть джампер 5V/3V3 — оставь на 5V. Питание ESP-CAM 5V.
"@ | Write-Host -ForegroundColor White
    Pause-Enter "Подключил все 4 провода (БЕЗ IO0↔GND пока), нажми Enter"
}

function Test-ModuleAlive ([string]$port) {
    Write-Step "Проверка что модуль жив — слушаю $port @ $BAUD"
    Write-Host "  Передёрни 5V (или нажми RST) в течение 12 секунд." -ForegroundColor White
    $p = $null
    $bootBytes = 0
    $isAt = $false
    try {
        $p = Open-SerialPort $port $BAUD
        $deadline = (Get-Date).AddSeconds(12)
        $accum = New-Object System.Collections.Generic.List[byte]
        while ((Get-Date) -lt $deadline) {
            $chunk = Read-Serial $p 200
            if ($chunk.Length -gt 0) {
                foreach ($b in $chunk) { $accum.Add($b) }
                $bootBytes += $chunk.Length
            }
        }
        $text = Format-SerialAscii $accum.ToArray()
        if ($bootBytes -gt 0) {
            Write-Ok ("Получено {0} байт — модуль жив" -f $bootBytes)
            $excerpt = ($text -split "`n" | Select-Object -First 3) -join ' / '
            Write-Hint "Начало: $excerpt"
            if ($text -match 'Bin version\(Wroom32\):1\.\d' -or $text -match 'at_customize') {
                Write-Warn2 "Похоже на stock Espressif AT firmware — она использует UART1 для AT, см. docs/at-firmware-uart-routing.md"
                Write-Hint "Если просто хочешь камеру в WiFi — продолжай, мы её перепишем"
                $isAt = $true
            } elseif ($text -match 'Camera Ready|CameraWebServer') {
                Write-Hint "На модуле уже CameraWebServer — будет перезалит"
            }
        } else {
            Write-Err2 "Тишина за 12 секунд."
            Write-Hint "Возможные причины: красный провод не контачит / белый не на U0T / VCC↔3V3 не замкнуты на адаптере"
            if (-not (Read-Confirm "Продолжить несмотря на это?" $false)) { throw "Модуль не отвечает" }
        }
    } finally {
        if ($p -and $p.IsOpen) { $p.Close() }
    }
    return @{ ByteCount = $bootBytes; IsAt = $isAt }
}

function Prompt-DownloadMode {
    Write-Step "Режим прошивки"
    @"
    Поставь перемычку IO0 ↔ GND на ESP32-CAM (любым свободным проводом).
    Затем передёрни 5V (вытащить красный, через секунду вставить обратно).

    Что произошло: IO0=LOW при reset → ROM bootloader входит в download mode
    и ждёт esptool. Никакого вывода в Serial при этом не будет — это норма.
"@ | Write-Host -ForegroundColor White
    Pause-Enter "IO0↔GND поставлен, 5V передёрнут — Enter"
}

function Invoke-Flash ([string]$cli, [string]$port, [string]$sketchDir) {
    Write-Step "Компилирую sketch ($sketchDir)"
    $compileOut = & $cli compile --fqbn $BOARD_FQBN --warnings none $sketchDir 2>&1
    $exit = $LASTEXITCODE
    if ($exit -ne 0) {
        $compileOut | ForEach-Object { Write-Host $_ }
        throw "Компиляция упала (exit $exit)"
    }
    $compileOut | Select-Object -Last 3 | ForEach-Object { Write-Hint $_ }
    Write-Ok "Скомпилировалось"

    Write-Step "Заливаю на $port"
    $uploadOut = & $cli upload --fqbn $BOARD_FQBN --port $port $sketchDir 2>&1
    $exit = $LASTEXITCODE
    $uploadOut | Select-String -Pattern 'Writing|Wrote|Hash|Hard resetting|Connecting|Failed|Permission|error' -CaseSensitive:$false |
        Select-Object -Last 6 | ForEach-Object { Write-Hint $_ }
    if ($exit -ne 0) {
        throw "esptool вернул $exit. Чек-лист: IO0↔GND стоит? 5V передёрнут после установки перемычки? COM-порт верный?"
    }
    Write-Ok "Прошивка залита"
}

function Wait-WiFiManagerIp ([string]$port, [int]$timeoutSec = 240) {
    Write-Step "Жду подключение к WiFi через captive portal"
    @"
    1. Сними перемычку IO0 ↔ GND
    2. Передёрни 5V на ESP-CAM
    3. С телефона/ноута найди WiFi-сеть 'ESP32-CAM-Setup' (без пароля)
    4. Подключись — должен открыться captive portal (или открой 192.168.4.1)
    5. Configure WiFi → выбери свою сеть, введи пароль → Save
    6. ESP-CAM сохранит, перезагрузится и подцепится к домашней сети.
       IP появится здесь автоматически.
"@ | Write-Host -ForegroundColor White

    $p = $null
    $ip = $null
    try {
        $p = Open-SerialPort $port $BAUD
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        $accum = New-Object System.Text.StringBuilder
        while ((Get-Date) -lt $deadline) {
            $chunk = Read-Serial $p 250
            if ($chunk.Length -gt 0) {
                $text = Format-SerialAscii $chunk
                [void]$accum.Append($text)
                # Печатаем только новые строки
                $text -split "`n" | Where-Object { $_ -ne '' } | ForEach-Object {
                    Write-Host "    [serial] $_" -ForegroundColor DarkGray
                }
                $all = $accum.ToString()
                # Шаблон IP в "Camera Ready! Use 'http://X.X.X.X' ..." или "STA IP Address: X.X.X.X"
                if ($all -match "http://(\d+\.\d+\.\d+\.\d+)") {
                    $cand = $Matches[1]
                    if ($cand -ne '192.168.4.1' -and $cand -ne '0.0.0.0') { $ip = $cand; break }
                }
                if ($all -match "STA IP Address:\s*(\d+\.\d+\.\d+\.\d+)") {
                    $cand = $Matches[1]
                    if ($cand -ne '192.168.4.1' -and $cand -ne '0.0.0.0') { $ip = $cand; break }
                }
            }
        }
    } finally {
        if ($p -and $p.IsOpen) { $p.Close() }
    }
    return $ip
}

# ============================================================
# Main
# ============================================================
try {
    Write-Host ""
    Write-Host "  ESP32-CAM CameraWebServer + WiFiManager — установка" -ForegroundColor White
    Write-Host "  ===================================================" -ForegroundColor DarkGray
    Write-Host ""

    # --- Sketch
    if (-not $SketchDir) { $SketchDir = $DEFAULT_SKETCH }
    if (-not (Test-Path (Join-Path $SketchDir 'CameraWebServer.ino'))) {
        throw "Не нашёл CameraWebServer.ino в $SketchDir. Положи sketch туда или укажи -SketchDir."
    }
    Write-Hint "Sketch: $SketchDir"

    # --- Tools
    $cli = Ensure-ArduinoCli
    Ensure-Esp32Core    $cli
    Ensure-WiFiManager  $cli

    # --- COM
    $port = Select-ComPort $ComPort
    Write-Ok "Использую $port"

    # --- Loopback
    if (-not $SkipLoopback) {
        if (Read-Confirm "Сделать loopback-тест адаптера (рекомендую при первой возне)?" $false) {
            Test-Loopback $port
        }
    }

    # --- Wiring
    Show-WiringGuide

    # --- Boot check
    if (-not $SkipBootCheck) {
        Test-ModuleAlive $port | Out-Null
    }

    # --- Download mode
    Prompt-DownloadMode

    # --- Flash
    Invoke-Flash $cli $port $SketchDir

    # --- Boot + IP
    $ip = Wait-WiFiManagerIp $port 240
    if (-not $ip) {
        Write-Warn2 "За 4 минуты IP не пойман — продолжай конфигурацию вручную. Бутлог должен быть выше."
        Write-Hint "Сеть 'ESP32-CAM-Setup' (без пароля), портал на http://192.168.4.1"
        exit 0
    }
    Write-Host ""
    Write-Ok "ESP-CAM готов: http://$ip"
    if (-not $NoBrowser) {
        if (Read-Confirm "Открыть в браузере?" $true) {
            Start-Process "http://$ip"
        }
    }
} catch {
    Write-Host ""
    Write-Err2 $_.Exception.Message
    Write-Hint "Полный stack: $($_.ScriptStackTrace)"
    exit 1
}
