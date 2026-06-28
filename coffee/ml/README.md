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
(`../../ml/`): вход 24×24×3, box-даунскейл ROI, нормализация в [0,1], int8. Уже
готовы и проверены в родителе:

- `../../ml/collect_session.py` — сбор кадров по сессиям с `/capture` + гейт по яркости
- `../../ml/train.py` — обучение + генерация `*_model.h` (int8)
- `../../ml/convert_to_header.py` — `.tflite` → C-заголовок
- `../../ml/make_bootstrap.py`, `review.py` — вспомогательные

Единственное отличие — **классы**: они заданы в локальном [`model.py`](model.py)
(`["empty", "coffee_black", "coffee_milk"]`). Скопируй сюда нужные скрипты из
`../../ml/` (они импортируют `model.py` из своей папки) и работай с этим
`model.py`.

## Порядок

```bash
cd coffee/ml
cp ../../ml/{collect_session.py,train.py,convert_to_header.py,requirements.txt} .

# venv для сбора (TF не нужен): numpy + Pillow + requests
python3 -m venv .venv && source .venv/bin/activate && pip install numpy Pillow requests

# 1. Прицелить ROI по горлу стакана и зафиксировать экспозицию (см. ../README.md),
#    снимать датасет ПРИ ТЕХ ЖЕ значениях экспозиции, что работает верификатор.
# 2. Снять кадры по сессиям (держишь одно состояние, снимаешь меткой):
python collect_session.py --host coffeecam.local --label empty        --count 300
python collect_session.py --host coffeecam.local --label coffee_black  --count 300
python collect_session.py --host coffeecam.local --label coffee_milk   --count 300

# 3. Обучить (TF нужен Python 3.11–3.12; нет wheel под 3.14):
/opt/homebrew/bin/python3.12 -m venv .venv-train && source .venv-train/bin/activate
pip install -r requirements.txt
python train.py --epochs 40       # -> drink_model.h
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
