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

# 0. Задать классы под своё меню в model.py → CLASS_NAMES, например полное меню:
#       CLASS_NAMES = ["empty", "coffee", "cappuccino", "latte", "tea", "cacao"]
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
python collect_session.py --host 192.168.1.101 --label empty      --count 400 --interval 0
python collect_session.py --host 192.168.1.101 --label coffee     --count 400 --interval 0
python collect_session.py --host 192.168.1.101 --label cappuccino --count 600 --interval 0   # хард-пара
python collect_session.py --host 192.168.1.101 --label latte      --count 600 --interval 0   # с капучино (по пене)
python collect_session.py --host 192.168.1.101 --label tea        --count 400 --interval 0
python collect_session.py --host 192.168.1.101 --label cacao      --count 500 --interval 0   # хард-пара с coffee (оба тёмные)
#    По строке на каждый класс из CLASS_NAMES; хард-парам дай больше кадров.
#    Повтори на РАЗНЫХ машинах / свете / стаканах — это и есть подготовка к «проду».

# 4. Обучить (TensorFlow нужен Python 3.11–3.12; под 3.14 wheel ещё нет):
python3.12 -m venv .venv-train && source .venv-train/bin/activate
pip install -r requirements.txt
python train.py --epochs 40        # → перезапишет ../CoffeeVerifier/drink_model.h

# 5. Перекомпилировать и перепрошить (см. корневой README):
arduino-cli compile --fqbn esp32:esp32:esp32cam ../CoffeeVerifier
arduino-cli upload  --fqbn esp32:esp32:esp32cam --port /dev/cu.usbserial-XXX ../CoffeeVerifier
```

## Проверить после прошивки

В Serial-логе (115200 бод) при старте прошивка печатает строку вида:

```
[verify] model ready, 6 classes, arena used XXXXX / 40960 bytes
```

- видишь свои классы и их число — значит новый `drink_model.h` подхватился
  (имена классов также появляются в `/metrics`, `/status` и вердиктах);
- `arena used` должен быть **меньше 40960**. Тензор-арена статическая, **40 КБ**
  (`CUP_TENSOR_ARENA_BYTES` в [`../CoffeeVerifier/config.h`](../CoffeeVerifier/config.h)).
  От добавления классов модель растёт чуть-чуть (только выходной слой), 40 КБ
  почти наверняка хватит. Если вместо строки выше видишь
  `AllocateTensors failed (arena too small?)` — подними значение (например
  `48 * 1024`) и перепрошей.

> Валидируй вживую (разные стаканы / свет / пенка), а не по `val_accuracy` —
> валидация из одной сессии оптимистична. Где путается (обычно хард-пары) —
> дособери кадров по этим классам и переобучи.
