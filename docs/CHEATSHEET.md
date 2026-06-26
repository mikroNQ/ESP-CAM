# Шпаргалка команд

Быстрый справочник по всем операциям проекта. Команды — для **macOS** (zsh/bash),
из корня репозитория, если не указано иное. Windows-варианты (PowerShell) — в
истории репозитория и в `scripts/*.ps1`.

**Текущие значения этой установки** (подставь свои, если отличается):

| Что | Значение |
|---|---|
| IP камеры `<cam-ip>` | `172.27.165.190` |
| IP компьютера `<pc-ip>` | `ipconfig getifaddr en0` |
| Порт `<port>` | `/dev/cu.usbserial-110` (CH340 → `cu.wchusbserial*`) |
| ROI | `18,37,124,12` |
| Экспозиция | `aec_value=1300`, `agc_gain=0`, AWB off |

> На macOS `curl` — настоящий curl, URL ниже можно дёргать прямо в терминале
> или открыть в браузере.

---

## 🔌 Прошивка (arduino-cli)

```bash
# 1) перемычка IO0<->GND, передёрнуть питание 5В, потом:
arduino-cli compile --fqbn esp32:esp32:esp32cam CameraWebServer
arduino-cli upload  --fqbn esp32:esp32:esp32cam --port /dev/cu.usbserial-110 CameraWebServer
# 2) снять перемычку IO0<->GND, передёрнуть питание
```

Или весь процесс интерактивно: `./scripts/setup-esp-cam.sh`

> Не шьётся (`No serial data received`, виснет на `Connecting...`)? → [`docs/macos-flashing-troubleshooting.md`](macos-flashing-troubleshooting.md).
> Первым делом освободи порт: `pkill -9 -f "arduino-cli upload"; pkill -9 -f esptool`.

Найти порт:
```bash
ls /dev/cu.*            # CH340 → cu.wchusbserial*, CP2102 → cu.SLAB_USBtoUART
arduino-cli board list
```

> Данные arduino-cli на macOS по умолчанию в `~/Library/Arduino15` —
> переменную окружения задавать не нужно.

---

## 📷 Настройка камеры (curl или открыть в браузере)

```text
http://<cam-ip>/status                                   # полный статус (камера + детектор)
http://<cam-ip>/detcfg                                   # конфиг детектора (прочитать)
http://<cam-ip>/capture?roi=1                            # кадр с зелёной рамкой ROI (прицеливание)

http://<cam-ip>/detcfg?roi_x=18&roi_y=37&roi_w=124&roi_h=12     # выставить ROI
http://<cam-ip>/detcfg?fixexp=1&aec_value=1300&agc_gain=0       # фикс. экспозиция
http://<cam-ip>/detcfg?fixexp=0                                 # вернуть авто-экспозицию
http://<cam-ip>/detcfg?enable=1                                 # детектор вкл (=0 выкл)
http://<cam-ip>/detcfg?host=<pc-ip>&port=9000                  # куда слать события
```

Найти камеру в сети (без mDNS): `./scripts/find-esp.sh`

---

## 🎞️ Сбор датасета (Python, из корня)

```bash
# один раз: venv (TF для сбора НЕ нужен)
python3 -m venv ml/.venv && source ml/.venv/bin/activate
pip install numpy Pillow requests
# держи нужное состояние сканера во время каждой команды:
python ml/collect_session.py --host <cam-ip> --label red_on   --min-v 75 --lock-aec 1300 --count 300
python ml/collect_session.py --host <cam-ip> --label white_on --min-v 85 --lock-aec 1300 --count 300
python ml/collect_session.py --host <cam-ip> --label off      --max-v 78 --lock-aec 1300 --count 300
```

Сколько собрано по классам:
```bash
for d in ml/data/*/; do echo "$(basename "$d"): $(ls "$d"*.png 2>/dev/null | wc -l | tr -d ' ')"; done
```

---

## 🧠 Обучение модели

TensorFlow требует Python 3.11–3.12 (под 3.14 wheel'ов нет) — отдельный venv:

```bash
brew install python@3.12
/opt/homebrew/bin/python3.12 -m venv ml/.venv-train && source ml/.venv-train/bin/activate
pip install -r ml/requirements.txt
python ml/train.py --epochs 40        # перезапишет CameraWebServer/led_model.h
# затем заново прошить (раздел «Прошивка»)
```

---

## 🖥️ Веб-дашборд (Go)

```bash
cd dashboard
go build -o esp-dashboard .
cd ..
./dashboard/esp-dashboard -esp=:9000 -http=:8080      # терминал не закрывать (Ctrl+C — стоп)
# открыть в браузере:  http://localhost:8080   (или http://<pc-ip>:8080)
# узнать IP этого компа:
ipconfig getifaddr en0
```

Остановить дашборд:
```bash
pkill -f esp-dashboard     # или Ctrl+C в его терминале
```

---

## 🔍 Диагностика

```bash
# средняя яркость/цвет ROI прямо сейчас (помогает подобрать экспозицию/гейты):
python3 -c "import requests,io,numpy as np; from PIL import Image; h='<cam-ip>'; r=requests.get(f'http://{h}/detcfg',timeout=6).json()['roi']; im=Image.open(io.BytesIO(requests.get(f'http://{h}/capture',timeout=10).content)).convert('RGB'); c=im.crop((r['x'],r['y'],r['x']+r['w'],r['y']+r['h'])); a=np.asarray(c,dtype=np.float32); print('R=%.0f G=%.0f B=%.0f V=%.0f'%(a[...,0].mean(),a[...,1].mean(),a[...,2].mean(),a.mean()))"

# снимок состояния дашборда (JSON):
curl -s http://localhost:8080/api/state

# здоровье ESP (ребут vs стопор, сигнал, память):
python3 -c "import requests; s=requests.get('http://<cam-ip>/status',timeout=8).json(); print('reset_reason=%s uptime_s=%s rssi=%s free_heap=%s'%(s['reset_reason'],s['uptime_s'],s['rssi'],s['free_heap']))"
# reset_reason: 1=POWERON 9=BROWNOUT(слабое питание!) 6/7=WDT 4=PANIC.
# uptime_s сбрасывается при «зависании» => был ребут (питание). rssi хуже -75 => слабый WiFi.
```

> `python3` выше — в активированном venv с numpy/Pillow/requests (раздел «Сбор датасета»).

Поймать IP камеры из Serial (если забыл) — `arduino-cli monitor -p /dev/cu.usbserial-110 -c baudrate=115200`
и передёрни питание; будет строка `Camera Ready! Use 'http://x.x.x.x'`.

---

## 🌿 Git

Bash-инструмент и `git` на этом Mac работают штатно:

```bash
git add <файлы>
git commit -m "сообщение"
git push origin main
```

---

## ⚙️ Где что менять (config)

| Параметр | Файл | Дефолт |
|---|---|---|
| ROI, экспозиция, endpoint | `CameraWebServer/config.h` | ROI 18,37,124,12 · aec 1300 |
| Задержка / дебаунс | `CameraWebServer/config.h` | `DET_SAMPLE_INTERVAL_MS=25`, `DET_DEBOUNCE_COUNT=1` |
| Порог уверенности | `CameraWebServer/config.h` | `DET_CONF_THRESHOLD=0.60` |
| Размер арены TFLite | `CameraWebServer/config.h` | `40*1024` |
| Классы / геометрия входа | `ml/model.py` | off/red_on/white_on, 24×24×3 |
| Буфер камеры (PSRAM/DRAM) | `CameraWebServer.ino` | PSRAM `fb_count=2` если есть, иначе DRAM `1` |
| Порты дашборда | флаги `-esp` / `-http` | `:9000` / `:8080` |
