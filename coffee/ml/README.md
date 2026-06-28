# Этап 2 — CNN на «с молоком / без» (опционально)

MVP (этап 1) **не требует обучения**: прошивка решает «налито/нет» по дельте от
baseline-кадра, а «молоко/без» — порогом яркости (`classify_milk()` в
`../CoffeeVerifier/cup_verifier.cpp`). Этот каталог — путь к этапу 2: крошечная
on-device CNN, которая различает напиток устойчивее порога (когда пенка/crema
сбивают яркость).

**Денежная ось остаётся на дельте** — ошибка CNN портит только точность типа
напитка, но не вердикт «списали, а кофе нет».

## Что переиспользовать

Архитектура и препроцессинг намеренно совпадают с родительским проектом
(`../../ml/`): вход 24×24×3, box-даунскейл ROI, нормализация в [0,1], int8.

- [`collect_session.py`](collect_session.py) — **уже здесь**, адаптирован под
  `/cupcfg` (родительский читал `/detcfg`). Сбор кадров по сессиям с гейтом по яркости.
- [`model.py`](model.py) — **уже здесь**, классы `["empty","coffee_black","coffee_milk"]`.
- `../../ml/train.py`, `../../ml/convert_to_header.py` — копируются из родителя
  (см. ниже), они импортируют `model.py` из текущей папки.

## Порядок

```bash
cd coffee/ml

# venv для СБОРА (TF не нужен): numpy + Pillow + requests
python3 -m venv .venv && source .venv/bin/activate && pip install numpy Pillow requests

# 1. Прицелить ROI по горлу стакана и зафиксировать экспозицию (см. ../README.md),
#    снимать датасет ПРИ ТЕХ ЖЕ значениях экспозиции, что работает верификатор:
#      curl "http://coffeecam.local/cupcfg?fixexp=1&aec_value=600&agc_gain=0"
# 2. Снять кадры по сессиям (держишь одно состояние, снимаешь меткой).
#    Сначала глянь живую яркость ROI в /metrics и подбери гейт --min-v/--max-v:
python collect_session.py --host coffeecam.local --label empty        --count 300
python collect_session.py --host coffeecam.local --label coffee_black  --count 300 --max-v 90
python collect_session.py --host coffeecam.local --label coffee_milk   --count 300 --min-v 110
#    Повтори на РАЗНЫХ машинах / свете / стаканах — это и есть подготовка к «проду».

# 3. Обучить (TF нужен Python 3.11–3.12; нет wheel под 3.14):
cp ../../ml/{train.py,convert_to_header.py,requirements.txt} .
# !! правка в train.py: HEADER_PATH должен указывать на ../CoffeeVerifier/drink_model.h
#    (родитель хардкодит ../CameraWebServer/led_model.h)
/opt/homebrew/bin/python3.12 -m venv .venv-train && source .venv-train/bin/activate
pip install -r requirements.txt
python train.py --epochs 40       # -> ../CoffeeVerifier/drink_model.h
```

## Вживить в прошивку

1. Положить сгенерированный `drink_model.h` в `../CoffeeVerifier/`.
2. В `cup_verifier.cpp` поднять TFLite-Micro интерпретатор (как в родительском
   `led_detector.cpp`: `MicroMutableOpResolver`, статическая арена) и заменить
   тело `classify_milk()` инференсом по ROI: argmax из `coffee_black` /
   `coffee_milk` → `false` / `true`. Сигнатура и вся логика вердикта выше —
   без изменений.
3. Перекомпилировать и перепрошить.

> Валидируй вживую (разные стаканы/свет/пенка), а не по `val_accuracy` —
> валидация из одной сессии оптимистична (грабли из родительского README).
