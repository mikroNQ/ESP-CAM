#!/usr/bin/env bash
#
# setup-esp-cam.sh — интерактивная прошивка ESP32-CAM (AI-Thinker)
#                    CameraWebServer + WiFiManager на macOS/Linux.
#
# Проводит по всей процедуре с нуля:
#   1. Проверяет arduino-cli (ставит подсказку про brew, если нет),
#      esp32 core 3.3.8, библиотеки WiFiManager и Chirale_TensorFlowLite.
#   2. Находит USB-TTL переходник (/dev/cu.*) и предлагает выбрать.
#   3. Показывает схему подключения, ждёт подтверждения.
#   4. (Опц.) Слушает бутлог — проверяет, что модуль жив, опознаёт stock AT.
#   5. Просит IO0<->GND + передёрнуть питание для входа в download mode.
#   6. Компилирует и заливает CameraWebServer.
#   7. Ловит IP от WiFiManager из Serial и предлагает открыть в браузере.
#
# Аналог Windows-скрипта scripts/setup-esp-cam.ps1 (raw-loopback-тест опущен —
# при необходимости проверяй адаптер через `arduino-cli monitor`).
#
# Использование:
#   ./scripts/setup-esp-cam.sh                       # автодетект порта
#   ./scripts/setup-esp-cam.sh --port /dev/cu.usbserial-110
#   ./scripts/setup-esp-cam.sh --skip-bootcheck      # сразу к прошивке
#   ./scripts/setup-esp-cam.sh --no-browser          # не открывать браузер
#
set -uo pipefail

# ============================================================
# Константы
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_SKETCH="$REPO_ROOT/CameraWebServer"
TOOLS_DIR="$REPO_ROOT/tools"
ESP32_INDEX_URL='https://espressif.github.io/arduino-esp32/package_esp32_index.json'
ESP32_CORE_VER='3.3.8'
WM_LIB_VER='2.0.17'
TFLITE_LIB='Chirale_TensorFlowLite'
BOARD_FQBN='esp32:esp32:esp32cam'
BAUD=115200

# ============================================================
# Флаги
# ============================================================
PORT=""
SKETCH_DIR=""
SKIP_BOOTCHECK=0
NO_BROWSER=0

usage() { grep '^#' "$0" | sed 's/^# \{0,1\}//' | sed '1d'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)           PORT="$2"; shift 2 ;;
    --sketch)         SKETCH_DIR="$2"; shift 2 ;;
    --skip-bootcheck) SKIP_BOOTCHECK=1; shift ;;
    --no-browser)     NO_BROWSER=1; shift ;;
    -h|--help)        usage; exit 0 ;;
    *) echo "Неизвестный аргумент: $1" >&2; exit 1 ;;
  esac
done

# ============================================================
# UI-хелперы (цвета только в TTY)
# ============================================================
if [[ -t 1 ]]; then
  C_CYAN=$(tput setaf 6 2>/dev/null || true); C_GREEN=$(tput setaf 2 2>/dev/null || true)
  C_YEL=$(tput setaf 3 2>/dev/null || true);  C_RED=$(tput setaf 1 2>/dev/null || true)
  C_DIM=$(tput setaf 8 2>/dev/null || true);  C_RST=$(tput sgr0 2>/dev/null || true)
else
  C_CYAN=""; C_GREEN=""; C_YEL=""; C_RED=""; C_DIM=""; C_RST=""
fi
step() { echo "${C_CYAN}==> $*${C_RST}"; }
ok()   { echo "${C_GREEN}[OK]  $*${C_RST}"; }
warn() { echo "${C_YEL}[!!]  $*${C_RST}"; }
err()  { echo "${C_RED}[ERR] $*${C_RST}" >&2; }
hint() { echo "${C_DIM}      $*${C_RST}"; }

die() { err "$*"; exit 1; }

# confirm <prompt> <default:Y|N>  → return 0 (yes) / 1 (no)
confirm() {
  local prompt="$1" def="${2:-Y}" hintstr ans
  if [[ "$def" == "Y" ]]; then hintstr="[Y/n]"; else hintstr="[y/N]"; fi
  while true; do
    read -r -p "$prompt $hintstr " ans
    if [[ -z "$ans" ]]; then [[ "$def" == "Y" ]] && return 0 || return 1; fi
    case "$(echo "$ans" | tr '[:upper:]' '[:lower:]')" in
      y|yes|д|да)  return 0 ;;
      n|no|н|нет)  return 1 ;;
    esac
  done
}

pause_enter() {
  local msg="${1:-нажми Enter когда готов}"
  echo ""
  read -r -p "    ${C_YEL}$msg ...${C_RST}" _
}

# ============================================================
# arduino-cli — детект и установка core/libs
# ============================================================
CLI=""
resolve_cli() {
  if [[ -x "$TOOLS_DIR/arduino-cli" ]]; then CLI="$TOOLS_DIR/arduino-cli"; return 0; fi
  if command -v arduino-cli >/dev/null 2>&1; then CLI="$(command -v arduino-cli)"; return 0; fi
  return 1
}

ensure_cli() {
  step "Проверяю arduino-cli"
  if ! resolve_cli; then
    err "arduino-cli не найден."
    if command -v brew >/dev/null 2>&1; then
      hint "Поставь: brew install arduino-cli — затем перезапусти скрипт."
    else
      hint "Поставь Homebrew (https://brew.sh), затем: brew install arduino-cli"
    fi
    exit 1
  fi
  ok "arduino-cli: $("$CLI" version 2>/dev/null | head -n1)"
}

ensure_core() {
  step "Проверяю ESP32 Arduino core $ESP32_CORE_VER"
  if "$CLI" core list 2>/dev/null | grep -Eq "^esp32:esp32[[:space:]]+$ESP32_CORE_VER"; then
    ok "esp32:esp32@$ESP32_CORE_VER уже установлен"; return
  fi
  hint "Обновляю индекс boards..."
  "$CLI" core update-index --additional-urls "$ESP32_INDEX_URL" >/dev/null 2>&1 || true
  hint "Ставлю esp32:esp32@$ESP32_CORE_VER (~250МБ, 2-5 мин)..."
  if ! "$CLI" core install "esp32:esp32@$ESP32_CORE_VER" --additional-urls "$ESP32_INDEX_URL"; then
    die "Не удалось поставить esp32 core"
  fi
  ok "esp32 core установлен"
}

ensure_lib() {  # $1 = "Name@ver" или "Name"
  local spec="$1" name="${1%@*}"
  step "Проверяю библиотеку $spec"
  if "$CLI" lib list 2>/dev/null | grep -Eq "^${name}[[:space:]]"; then
    ok "$name уже стоит"; return
  fi
  hint "Ставлю $spec..."
  if ! "$CLI" lib install "$spec"; then die "Не удалось поставить $spec"; fi
  ok "$name установлен"
}

# ============================================================
# Детект serial-порта (/dev/cu.*)
# ============================================================
chip_of() {  # по имени устройства угадываем чип
  case "$1" in
    *wchusbserial*|*ch34*|*CH34*)      echo "CH340" ;;
    *SLAB_USBtoUART*|*cp210*|*CP210*)  echo "CP2102" ;;
    *usbserial-FT*|*FTDI*)             echo "FTDI" ;;
    *usbmodem*)                        echo "USB-CDC (родной)" ;;
    *PL2303*|*usbserial-1*)            echo "PL2303?/generic" ;;
    *)                                 echo "serial" ;;
  esac
}

list_ports() {  # печатает по строке: "<dev>\t<chip>"
  local d
  for d in /dev/cu.*; do
    [[ -e "$d" ]] || continue
    case "$d" in
      *Bluetooth-Incoming-Port|*debug-console|*wlan-debug) continue ;;
    esac
    printf '%s\t%s\n' "$d" "$(chip_of "$d")"
  done
}

select_port() {
  step "Поиск USB-TTL переходника (/dev/cu.*)"
  if [[ -n "$PORT" ]]; then hint "Использую указанный: $PORT"; return; fi

  local ports=()
  while IFS= read -r line; do [[ -n "$line" ]] && ports+=("$line"); done < <(list_ports)

  if [[ ${#ports[@]} -eq 0 ]]; then
    err "Ни одного serial-порта не найдено. Воткни USB-TTL и нажми Enter."
    pause_enter "USB-TTL воткнут"
    ports=()
    while IFS= read -r line; do [[ -n "$line" ]] && ports+=("$line"); done < <(list_ports)
    [[ ${#ports[@]} -eq 0 ]] && die "Так и не нашёл /dev/cu.* (поставлен ли драйвер CH340/CP210x?)"
  fi

  if [[ ${#ports[@]} -eq 1 ]]; then
    PORT="${ports[0]%%$'\t'*}"
    ok "Найден один: $PORT ($(chip_of "$PORT"))"
    confirm "Использовать $PORT?" Y || die "Отменено пользователем"
    return
  fi

  echo ""
  echo "  Найдено несколько портов:"
  local i=1 dev chip
  for line in "${ports[@]}"; do
    dev="${line%%$'\t'*}"; chip="${line#*$'\t'}"
    printf "    [%d] %-32s %s\n" "$i" "$dev" "$chip"
    i=$((i+1))
  done
  local idx
  while true; do
    read -r -p "  Какой? (номер) " idx
    if [[ "$idx" =~ ^[0-9]+$ ]] && (( idx >= 1 && idx <= ${#ports[@]} )); then
      PORT="${ports[$((idx-1))]%%$'\t'*}"; return
    fi
    warn "Введи число от 1 до ${#ports[@]}"
  done
}

# ============================================================
# Чтение Serial через arduino-cli monitor (с таймаутом).
# monitor_capture <port> <timeout_sec> <egrep_pat> <live:0|1>
# Результат — в глобалах (НЕ через $(...) — иначе теряются в подоболочке):
#   MONITOR_MATCH  — первая строка, совпавшая с паттерном (или пусто)
#   MONITOR_BYTES  — сколько байт прочитано из Serial
#   MONITOR_FILE   — путь к временному логу (удалить вызывающему)
# При live=1 печатает новые строки бутлога в stderr.
# ============================================================
MONITOR_MATCH=""; MONITOR_BYTES=0; MONITOR_FILE=""
monitor_capture() {
  local port="$1" tmo="$2" pat="$3" live="${4:-0}"
  local tmpf; tmpf="$(mktemp -t espcam)"
  "$CLI" monitor -p "$port" -c "baudrate=$BAUD" -q >"$tmpf" 2>/dev/null &
  local mon=$!
  local i=0 shown=0 total match=""
  while (( i < tmo )); do
    kill -0 "$mon" 2>/dev/null || break
    total=$(wc -l <"$tmpf" 2>/dev/null | tr -d ' ')
    total=${total:-0}
    if [[ "$live" == "1" ]] && (( total > shown )); then
      sed -n "$((shown+1)),${total}p" "$tmpf" 2>/dev/null \
        | while IFS= read -r ln; do echo "${C_DIM}    [serial] $ln${C_RST}" >&2; done
      shown=$total
    fi
    match=$(grep -Eom1 "$pat" "$tmpf" 2>/dev/null || true)
    [[ -n "$match" ]] && break
    sleep 1; i=$((i+1))
  done
  kill "$mon" 2>/dev/null || true
  wait "$mon" 2>/dev/null || true
  # на случай, если совпадение появилось в последний момент
  [[ -z "$match" ]] && match=$(grep -Eom1 "$pat" "$tmpf" 2>/dev/null || true)
  MONITOR_MATCH="$match"
  MONITOR_BYTES=$(wc -c <"$tmpf" 2>/dev/null | tr -d ' ')
  MONITOR_FILE="$tmpf"
}

# ============================================================
# Шаги пайплайна
# ============================================================
show_wiring() {
  step "Распиновка USB-TTL <-> ESP32-CAM (для прошивки и работы)"
  cat <<'EOF'
    ┌────────────────┐         ┌──────────────────┐
    │ USB-TTL adapter│         │   ESP32-CAM      │
    ├────────────────┤         ├──────────────────┤
    │ 5V             │ ──────▶ │ 5V               │
    │ GND            │ ──────▶ │ GND              │
    │ TXD            │ ──────▶ │ U0R  (GPIO3, RX) │
    │ RXD            │ ◀────── │ U0T  (GPIO1, TX) │
    └────────────────┘         └──────────────────┘

    Если на адаптере есть отдельный пин VCC (как на HW-597) — замкни VCC↔3V3
    на самом адаптере. Это включает 3.3V логику на TXD/RXD, иначе ESP-CAM
    не примет сигнал.

    Если на адаптере есть джампер 5V/3V3 — оставь на 5V (питание ESP-CAM).
EOF
  pause_enter "Подключил все 4 провода (БЕЗ IO0↔GND пока), нажми Enter"
}

test_module_alive() {
  step "Проверка что модуль жив — слушаю $PORT @ $BAUD (12с)"
  echo "  Передёрни 5V (или нажми RST) в течение 12 секунд."
  monitor_capture "$PORT" 12 'Camera Ready|CameraWebServer|Bin version|at_customize' 1
  local bytes="${MONITOR_BYTES:-0}"
  if (( bytes > 0 )); then
    ok "Получено $bytes байт — модуль жив"
    if grep -Eq 'Bin version\(Wroom32\):1\.|at_customize' "$MONITOR_FILE" 2>/dev/null; then
      warn "Похоже на stock Espressif AT firmware — UART1 для AT, см. docs/at-firmware-uart-routing.md"
      hint "Если просто хочешь камеру в WiFi — продолжай, мы её перепишем"
    elif grep -Eq 'Camera Ready|CameraWebServer' "$MONITOR_FILE" 2>/dev/null; then
      hint "На модуле уже CameraWebServer — будет перезалит"
    fi
  else
    err "Тишина за 12 секунд."
    hint "Причины: 5V/GND не контачат / RXD не на U0T / VCC↔3V3 не замкнуты на адаптере"
    confirm "Продолжить несмотря на это?" N || die "Модуль не отвечает"
  fi
  rm -f "$MONITOR_FILE" 2>/dev/null || true
}

prompt_download_mode() {
  step "Режим прошивки"
  cat <<'EOF'
    Поставь перемычку IO0 ↔ GND на ESP32-CAM (любым свободным проводом).
    Затем передёрни 5V (вытащить, через секунду вставить обратно).

    IO0=LOW при reset → ROM bootloader входит в download mode и ждёт esptool.
    Вывода в Serial при этом не будет — это норма.
EOF
  pause_enter "IO0↔GND поставлен, 5V передёрнут — Enter"
}

do_flash() {
  step "Компилирую sketch ($SKETCH_DIR)"
  if ! "$CLI" compile --fqbn "$BOARD_FQBN" --warnings none "$SKETCH_DIR"; then
    die "Компиляция упала"
  fi
  ok "Скомпилировалось"

  step "Заливаю на $PORT"
  if ! "$CLI" upload --fqbn "$BOARD_FQBN" --port "$PORT" "$SKETCH_DIR"; then
    die "esptool вернул ошибку. Чек-лист: IO0↔GND стоит? 5V передёрнут после установки перемычки? Порт верный?"
  fi
  ok "Прошивка залита"
}

# Результат — в глобале FOUND_IP.
FOUND_IP=""
wait_for_ip() {
  step "Жду подключение к WiFi через captive portal (до 4 минут)"
  cat <<'EOF'
    1. Сними перемычку IO0 ↔ GND
    2. Передёрни 5V на ESP-CAM
    3. С телефона/ноута найди WiFi-сеть 'ESP32-CAM-Setup' (без пароля)
    4. Подключись — откроется captive portal (или зайди на http://192.168.4.1)
    5. Configure WiFi → выбери свою сеть, введи пароль → Save
    6. ESP-CAM сохранит, перезагрузится и подцепится к домашней сети.
       IP появится здесь автоматически.
EOF
  # Берём первый http://X.X.X.X, отличный от портала 192.168.4.1.
  monitor_capture "$PORT" 240 'http://[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+|STA IP Address:[[:space:]]*[0-9.]+' 1
  rm -f "${MONITOR_FILE:-}" 2>/dev/null || true
  local ip
  ip=$(echo "$MONITOR_MATCH" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
  if [[ "$ip" == "192.168.4.1" || "$ip" == "0.0.0.0" ]]; then ip=""; fi
  FOUND_IP="$ip"
}

# ============================================================
# Main
# ============================================================
echo ""
echo "  ESP32-CAM CameraWebServer + WiFiManager — установка (macOS/Linux)"
echo "  ================================================================"
echo ""

[[ -z "$SKETCH_DIR" ]] && SKETCH_DIR="$DEFAULT_SKETCH"
[[ -f "$SKETCH_DIR/CameraWebServer.ino" ]] || \
  die "Не нашёл CameraWebServer.ino в $SKETCH_DIR. Укажи --sketch."
hint "Sketch: $SKETCH_DIR"

ensure_cli
ensure_core
ensure_lib "WiFiManager@$WM_LIB_VER"
ensure_lib "$TFLITE_LIB"

select_port
ok "Использую $PORT"

show_wiring

if [[ $SKIP_BOOTCHECK -eq 0 ]]; then
  test_module_alive
fi

prompt_download_mode
do_flash

wait_for_ip
ip="$FOUND_IP"
if [[ -z "$ip" ]]; then
  echo ""
  warn "За 4 минуты IP не пойман — продолжай конфигурацию вручную (бутлог выше)."
  hint "Сеть 'ESP32-CAM-Setup' (без пароля), портал http://192.168.4.1"
  exit 0
fi

echo ""
ok "ESP-CAM готов: http://$ip"
if [[ $NO_BROWSER -eq 0 ]]; then
  if confirm "Открыть в браузере?" Y; then
    open "http://$ip" 2>/dev/null || xdg-open "http://$ip" 2>/dev/null || true
  fi
fi
