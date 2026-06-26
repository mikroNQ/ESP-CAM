#!/usr/bin/env bash
#
# find-esp.sh — находит IP ESP32-CAM в локальной сети (macOS/Linux, без mDNS).
#
# DHCP выдаёт камере плавающий адрес, а mDNS (scanner.local) не всегда
# резолвится между сегментами сети. Скрипт сканирует /24 и опознаёт камеру по
# уникальным полям нашей прошивки в /status (det_state, det_roi).
#
# Зависимости: curl, python3 (только для красивого вывода — не обязателен).
#
# Использование:
#   ./scripts/find-esp.sh                     # автоподсеть из активного интерфейса
#   ./scripts/find-esp.sh --subnet 192.168.1  # явный /24
#   ./scripts/find-esp.sh --open              # открыть найденный дашборд в браузере
#   ip=$(./scripts/find-esp.sh --quiet)       # только IP в stdout (для скриптов)
#
set -u

SUBNET=""
OPEN=0
QUIET=0

usage() { grep '^#' "$0" | sed 's/^# \{0,1\}//' | sed '1d'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --subnet) SUBNET="${2%.}"; shift 2 ;;
    --open)   OPEN=1; shift ;;
    --quiet|-q) QUIET=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Неизвестный аргумент: $1" >&2; exit 1 ;;
  esac
done

# Сообщения идут в stderr, чтобы --quiet оставлял в stdout только IP.
info() { [[ $QUIET -eq 1 ]] || echo "$@" >&2; }

# --- Определить подсеть ---------------------------------------------------
# Кандидаты: интерфейс маршрута по умолчанию (если у него есть IPv4 от DHCP) +
# обычные Wi-Fi/Ethernet (en0..en3). VPN-туннели (utun*) в ipconfig getifaddr
# не светятся, поэтому при активном VPN автоматически берётся реальный LAN.
if [[ -z "$SUBNET" ]]; then
  iface=""
  myip=""
  def_iface=$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')
  for cand in "$def_iface" en0 en1 en2 en3; do
    [[ -z "$cand" ]] && continue
    cip=$(ipconfig getifaddr "$cand" 2>/dev/null || true)
    if [[ -n "$cip" ]]; then iface="$cand"; myip="$cip"; break; fi
  done
  if [[ -z "$myip" ]]; then
    echo "Не нашёл интерфейс с IPv4. Укажи подсеть вручную: --subnet 192.168.1" >&2
    exit 1
  fi
  SUBNET="${myip%.*}"
  info "Интерфейс $iface, мой IP $myip → сканирую ${SUBNET}.0/24"
else
  info "Сканирую ${SUBNET}.0/24"
fi

info "Опрашиваю ${SUBNET}.1-254 на сигнатуру прошивки (/status, det_state/det_roi)…"

# --- Параллельный опрос /status ------------------------------------------
# Для каждого адреса дёргаем /status и проверяем поля прошивки.
probe() {
  local ip="$1" body
  body=$(curl -fsS --max-time 2 "http://$ip/status" 2>/dev/null) || return
  case "$body" in
    *'"det_state"'*'"det_roi"'* | *'"det_roi"'*'"det_state"'*) echo "$ip" ;;
  esac
}
export -f probe

hit=$(seq 1 254 | sed "s#^#${SUBNET}.#" \
        | xargs -P 64 -I{} bash -c 'probe "$1"' _ {} 2>/dev/null \
        | head -n1 || true)

if [[ -z "$hit" ]]; then
  echo "ESP не найдена (ни один хост не отдал /status с det_state/det_roi)." >&2
  echo "Камера выключена, в другой подсети или ещё не подняла WiFi." >&2
  exit 1
fi

if [[ $QUIET -eq 1 ]]; then
  echo "$hit"
else
  echo "" >&2
  echo "ESP найдена: http://$hit/"
  if command -v python3 >/dev/null 2>&1; then
    curl -fsS --max-time 3 "http://$hit/status" 2>/dev/null \
      | python3 -c 'import sys,json
try:
    j=json.load(sys.stdin)
    print("  det_state=%s  rssi=%s  uptime_s=%s  tcp_connected=%s" % (
        j.get("det_state"), j.get("rssi"), j.get("uptime_s"), j.get("tcp_connected")))
except Exception:
    pass' || true
  fi
  echo "  Прицел ROI:  http://$hit/capture?roi=1"
fi

if [[ $OPEN -eq 1 ]]; then
  open "http://$hit/" 2>/dev/null || xdg-open "http://$hit/" 2>/dev/null || true
fi
