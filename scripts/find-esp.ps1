<#
.SYNOPSIS
    Находит IP ESP32-CAM в локальной сети без mDNS.

.DESCRIPTION
    DHCP выдаёт камере плавающий адрес, а mDNS (`scanner.local`) не работает,
    когда ESP сидит на Wi-Fi, а ПК — на проводе (корпоративные сети режут
    multicast между сегментами). Этот скрипт сканирует подсеть и опознаёт камеру
    по уникальным полям нашей прошивки в /status (`det_state`, `det_roi`).

.PARAMETER Subnet
    Префикс /24 для скана, напр. "172.27.166". По умолчанию перебирает все /24
    из подсети проводного интерфейса (172.27.x по умолчанию).

.PARAMETER Open
    Сразу открыть найденный дашборд в браузере.

.EXAMPLE
    .\find-esp.ps1
    .\find-esp.ps1 -Subnet 172.27.166 -Open
    $ip = .\find-esp.ps1 -Quiet ; curl "http://$ip/capture?roi=1" -o frame.jpg
#>
[CmdletBinding()]
param(
    [string]$Subnet,
    [switch]$Open,
    [switch]$Quiet
)

function Write-Info($m) { if (-not $Quiet) { Write-Host $m -ForegroundColor Cyan } }

# Список /24 для перебора.
if ($Subnet) {
    $prefixes = @($Subnet.TrimEnd('.'))
} else {
    # Берём IPv4 локального LAN (172.27.x) и перебираем .164-.167 (это /22).
    $lan = Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -like '172.27.*' } |
        Select-Object -First 1
    if (-not $lan) {
        Write-Error "Не нашёл интерфейс 172.27.x — укажи -Subnet вручную."
        exit 1
    }
    $prefixes = 164..167 | ForEach-Object { "172.27.$_" }
}

Write-Info "Сканирую: $($prefixes -join ', ').* (порт 80)…"

# 1) Быстрый скан открытого 80-го порта.
$candidates = foreach ($p in $prefixes) {
    0..255 | ForEach-Object -Parallel {
        $ip = "$using:p.$_"
        if (Test-Connection -TargetName $ip -TcpPort 80 -TimeoutSeconds 1 -Quiet) { $ip }
    } -ThrottleLimit 80
}

if (-not $candidates) {
    Write-Error "Ни одного хоста с открытым 80-м портом не найдено."
    exit 1
}

Write-Info "Кандидатов с портом 80: $($candidates.Count). Проверяю /status…"

# 2) Опознаём камеру по сигнатуре нашей прошивки.
$hit = $candidates | ForEach-Object -Parallel {
    $ip = $_
    try {
        $body = (Invoke-WebRequest "http://$ip/status" -TimeoutSec 3 -UseBasicParsing).Content
        if ($body -match '"det_state"' -and $body -match '"det_roi"') { $ip }
    } catch { }
} -ThrottleLimit 40 | Select-Object -First 1

if (-not $hit) {
    Write-Error "ESP не найдена (нет /status с полями det_state/det_roi). Камера выключена или в другой подсети."
    exit 1
}

if ($Quiet) {
    Write-Output $hit
} else {
    $j = (Invoke-WebRequest "http://$hit/status" -TimeoutSec 3 -UseBasicParsing).Content | ConvertFrom-Json
    Write-Host "`nESP найдена: http://$hit/" -ForegroundColor Green
    Write-Host ("  det_state={0}  rssi={1}  uptime_s={2}  tcp_connected={3}" -f `
        $j.det_state, $j.rssi, $j.uptime_s, $j.tcp_connected)
    Write-Host "  Прицел ROI:  http://$hit/capture?roi=1"
}

if ($Open) { Start-Process "http://$hit/" }
