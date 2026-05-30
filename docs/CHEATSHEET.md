# Шпаргалка команд

Быстрый справочник по всем операциям проекта. Команды — для **PowerShell**
(Windows), из корня репозитория, если не указано иное.

**Текущие значения этой установки** (подставь свои, если отличается):

| Что | Значение |
|---|---|
| IP камеры `<cam-ip>` | `172.27.165.190` |
| IP компьютера `<pc-ip>` | `172.27.165.172` |
| COM-порт `<port>` | `COM3` |
| ROI | `18,37,124,12` |
| Экспозиция | `aec_value=1300`, `agc_gain=0`, AWB off |

> В PowerShell `curl` — это псевдоним `Invoke-WebRequest` (другой синтаксис). Для
> URL ниже проще **открыть их в браузере**, либо вызывать реальный `curl.exe`.

---

## 🔌 Прошивка (arduino-cli)

```powershell
# 1) перемычка IO0<->GND, передёрнуть питание 5В, потом:
$env:ARDUINO_DIRECTORIES_DATA='C:\Users\ChueshovV\AppData\Local\Arduino15'
.\tools\arduino-cli.exe compile --fqbn esp32:esp32:esp32cam CameraWebServer
.\tools\arduino-cli.exe upload  --fqbn esp32:esp32:esp32cam --port COM3 CameraWebServer
# 2) снять перемычку IO0<->GND, передёрнуть питание
```

Найти COM-порт:
```powershell
Get-PnpDevice -Class Ports -PresentOnly | Where-Object FriendlyName -match 'CH340|CP210|FTDI'
```

---

## 📷 Настройка камеры (открыть в браузере или `curl.exe`)

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

---

## 🎞️ Сбор датасета (Python, из корня)

```powershell
# один раз: pip install -r ml\requirements.txt
# держи нужное состояние сканера во время каждой команды:
py ml\collect_session.py --host <cam-ip> --label red_on   --min-v 75 --lock-aec 1300 --count 300
py ml\collect_session.py --host <cam-ip> --label white_on --min-v 85 --lock-aec 1300 --count 300
py ml\collect_session.py --host <cam-ip> --label off      --max-v 78 --lock-aec 1300 --count 300
```

Сколько собрано по классам:
```powershell
Get-ChildItem ml\data -Directory | ForEach-Object { "$($_.Name): $((Get-ChildItem $_.FullName -Filter *.png).Count)" }
```

---

## 🧠 Обучение модели

```powershell
py ml\train.py --epochs 40        # перезапишет CameraWebServer\led_model.h
# затем заново прошить (раздел «Прошивка»)
```

---

## 🖥️ Веб-дашборд (Go)

```powershell
cd dashboard
go build -o esp-dashboard.exe .
cd ..
.\dashboard\esp-dashboard.exe -esp=:9000 -http=:8080      # чёрное окно не закрывать
# открыть в браузере:  http://localhost:8080   (или http://<pc-ip>:8080)
# узнать IP этого ПК:
(Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -notlike '169.*').IPAddress
```

Остановить дашборд:
```powershell
Stop-Process -Name esp-dashboard -Force
```

---

## 🔍 Диагностика

```powershell
# средняя яркость/цвет ROI прямо сейчас (помогает подобрать экспозицию/гейты):
py -c "import requests,io,numpy as np; from PIL import Image; h='<cam-ip>'; r=requests.get(f'http://{h}/detcfg',timeout=6).json()['roi']; im=Image.open(io.BytesIO(requests.get(f'http://{h}/capture',timeout=10).content)).convert('RGB'); c=im.crop((r['x'],r['y'],r['x']+r['w'],r['y']+r['h'])); a=np.asarray(c,dtype=np.float32); print('R=%.0f G=%.0f B=%.0f V=%.0f'%(a[...,0].mean(),a[...,1].mean(),a[...,2].mean(),a.mean()))"

# снимок состояния дашборда (JSON):
(Invoke-WebRequest 'http://localhost:8080/api/state' -UseBasicParsing).Content

# слушать сырые TCP-события без дашборда (нужен nc/ncat), либо смотреть /api/state
```

Поймать IP камеры с порта (если забыл) — открой Serial Monitor на `<port>` @115200
и передёрни питание; будет строка `Camera Ready! Use 'http://x.x.x.x'`.

---

## 🌿 Git (особенность окружения)

В этом окружении штатный `git push` ломается (GCM + msys). Рабочая команда —
сбросить список хелперов и оставить `wincred`:

```powershell
git add <файлы>
git commit -m "сообщение"
git -c credential.helper= -c credential.helper=wincred push origin main
```

> Bash-инструмент тут крашится — все git/shell-команды через PowerShell.

---

## ⚙️ Где что менять (config)

| Параметр | Файл | Дефолт |
|---|---|---|
| ROI, экспозиция, endpoint | `CameraWebServer/config.h` | ROI 18,37,124,12 · aec 1300 |
| Задержка / дебаунс | `CameraWebServer/config.h` | `DET_SAMPLE_INTERVAL_MS=25`, `DET_DEBOUNCE_COUNT=1` |
| Порог уверенности | `CameraWebServer/config.h` | `DET_CONF_THRESHOLD=0.60` |
| Размер арены TFLite | `CameraWebServer/config.h` | `40*1024` |
| Классы / геометрия входа | `ml/model.py` | off/red_on/white_on, 24×24×3 |
| Порты дашборда | флаги `-esp` / `-http` | `:9000` / `:8080` |
