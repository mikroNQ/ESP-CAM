# CNN-классификатор напитка — обучение и дообучение

Прошивка решает «налито / не налито» (денежная ось) по дельте ROI от
baseline-кадра — **без ML**. Тип напитка определяет крошечная on-device CNN:
в текущей сборке она **уже вшита** в прошивку (`classify_drink()` в
[`../CoffeeVerifier/cup_verifier.cpp`](../CoffeeVerifier/cup_verifier.cpp), веса в
[`../CoffeeVerifier/drink_model.h`](../CoffeeVerifier/drink_model.h)) и вызывается
на устоявшемся кадре. Этот каталог — как переобучить эту CNN под свои
стаканы / свет / напитки.

**Денежная ось остаётся на дельте** — ошибка CNN портит только точность типа
напитка, но не вердикт «списали, а кофе нет».

> Шипнутая модель — POC на 2 класса (`empty` / `cacao`, см. `CLASS_NAMES` в
> [`model.py`](model.py) и `DRINK_MODEL_NUM_CLASSES` в `drink_model.h`). Это
> заглушка для проверки пайплайна; под реальное меню её надо переобучить (ниже).

## Файлы (всё локально, ничего копировать не нужно)

- [`collect_session.py`](collect_session.py) — сбор кадров по сессиям через
  `/cupcfg` + `/capture`, с гейтом по яркости. Пишет в `data/<label>/*.png`.
- [`model.py`](model.py) — архитектура (вход 24×24×3, int8) и `CLASS_NAMES`.
- [`train.py`](train.py) — обучает по `data/<label>/`, квантует в int8 TFLite и
  пишет заголовок прямо в `../CoffeeVerifier/drink_model.h`.
- [`convert_to_header.py`](convert_to_header.py) — `.tflite` → C-заголовок.
- [`requirements.txt`](requirements.txt) — зависимости обучения (TensorFlow и пр.).

## Цикл переобучения

```bash
cd coffee/ml

# 0. Задать классы под своё меню в model.py → CLASS_NAMES.
#    Метки сбора и порядок выхода берутся отсюда. Прошивка подхватывает имена
#    классов и их число из сгенерированного drink_model.h — править C++ НЕ нужно.

# 1. Прицелить ROI и зафиксировать экспозицию (см. ../README.md). Снимать датасет
#    ПРИ ТЕХ ЖЕ значениях экспозиции, при которых работает верификатор:
#      curl "http://coffeecam.local/cupcfg?fixexp=1&aec_value=600&agc_gain=0"

# 2. venv для СБОРА (TensorFlow не нужен): numpy + Pillow + requests
python3 -m venv .venv && source .venv/bin/activate && pip install numpy Pillow requests

# 3. Снять кадры по сессиям — одну метку за сессию, метки = CLASS_NAMES.
#    Снимай ФИНАЛЬНЫЙ налитый уровень (после settle), не переходные кадры налива.
#    Используй IP, а не coffeecam.local (.local на macOS даёт ~5с mDNS-таймаут).
python collect_session.py --host 192.168.1.101 --label empty --count 400 --interval 0
python collect_session.py --host 192.168.1.101 --label cacao --count 500 --interval 0
#    ... по строке на каждый класс из CLASS_NAMES.
#    Повтори на РАЗНЫХ машинах / свете / стаканах — это и есть подготовка к «проду».

# 4. Обучить (TensorFlow нужен Python 3.11–3.12; под 3.14 wheel ещё нет):
python3.12 -m venv .venv-train && source .venv-train/bin/activate
pip install -r requirements.txt
python train.py --epochs 40        # → перезапишет ../CoffeeVerifier/drink_model.h

# 5. Перекомпилировать и перепрошить (см. корневой README):
arduino-cli compile --fqbn esp32:esp32:esp32cam ../CoffeeVerifier
arduino-cli upload  --fqbn esp32:esp32:esp32cam --port /dev/cu.usbserial-XXX ../CoffeeVerifier
```

Новые классы попадают в `/metrics`, `/status` и вердикты автоматически — прошивка
читает имена классов и их число из `drink_model.h`, никаких правок кода.

> Валидируй вживую (разные стаканы / свет / пенка), а не по `val_accuracy` —
> валидация из одной сессии оптимистична.
