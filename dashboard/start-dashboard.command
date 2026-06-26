#!/bin/bash
# Двойной клик по этому файлу запускает дашборд на macOS.
# (macOS-аналог двойного клика по esp-dashboard.exe на Windows.)
cd "$(dirname "$0")" || exit 1

# Если бинарь ещё не собран — собрать (нужен Go: brew install go).
if [ ! -x ./esp-dashboard ]; then
  echo "Собираю esp-dashboard (один раз)…"
  if ! command -v go >/dev/null 2>&1; then
    echo "Go не установлен. Поставь: brew install go — и запусти файл снова."
    echo "Нажми Enter для выхода."; read -r _; exit 1
  fi
  if ! go build -o esp-dashboard .; then
    echo "Сборка не удалась. Нажми Enter для выхода."; read -r _; exit 1
  fi
fi

echo "============================================================"
echo "  Дашборд запущен. НЕ ЗАКРЫВАЙ это окно."
echo "  Открой в браузере:  http://localhost:8080"
echo "  Остановить — закрой окно или нажми Ctrl+C."
echo "============================================================"
exec ./esp-dashboard -esp=:9000 -http=:8080
