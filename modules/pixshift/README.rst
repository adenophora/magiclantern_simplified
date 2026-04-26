# pixshift — Pixel Shift Resolution для Magic Lantern

## Что это

Модуль снимает серию из 2 или 4 кадров с микро-смещением между ними,
имитируя pixel shift как у Pentax/Olympus. Итоговый merge — в Lightroom,
Photoshop или любым другим инструментом на ПК.

## Как это работает на 50D

50D не имеет сенсорного актуатора (IBIS или пьезо), поэтому смещение
реализуется через фокусный мотор EF-объектива:

```
Кадр 0        Кадр 1         Кадр 2         Кадр 3
[без сдвига]  [+N шагов AF]  [+N шагов AF]  [+N шагов AF]
              ↑ lens motor   ↑ lens motor   ↑ lens motor
```

После серии модуль возвращает фокус на исходную позицию.

## Режимы

### Auto (lens motor)
- Требует EF-объектив с мотором AF
- Сдвиг управляется параметром `Motor Step` (1–10 шагов)
- Лучше всего с фиксами (50mm, 100mm) на штативе

### Manual (tripod)
- Для мануальных/адаптированных объективов
- Между кадрами модуль ждёт полунажатия кнопки спуска
- Сдвиг камеры делается вручную (микрометрический штатив)

## Параметры

| Параметр | Диапазон | По умолч. | Описание |
|---|---|---|---|
| Frames | 2–4 | 4 | Количество кадров |
| Shift Mode | Auto/Manual | Auto | Способ сдвига |
| Motor Step | 1–10 | 2 | Шаги фокус-мотора |
| Stabilization Delay | 100–2000 мс | 500 мс | Пауза после сдвига |

## Меню

```
Shoot → Pixel Shift
              ├─ Shoot Now
              ├─ Frames          [2..4]
              ├─ Shift Mode      [Auto / Manual]
              ├─ Motor Step      [1..10]
              └─ Stabilization Delay [100..2000 ms]
```

## Сборка

```bash
git clone https://github.com/reticulatedpines/magiclantern_simplified
cp -r pixshift/ magiclantern_simplified/modules/
cd magiclantern_simplified/modules/pixshift
make ML_PATH=../../
```

## Постпроцессинг

После съёмки у вас будет серия RAW-файлов:
- `IMG_0001.CR2` — базовый кадр
- `IMG_0002.CR2` — сдвиг +1
- `IMG_0003.CR2` — сдвиг +2
- `IMG_0004.CR2` — сдвиг +3

**Lightroom / ACR**: Выделить все → Photo Merge → HDR (с выровненными кадрами)
или любой SR-стек (Topaz Gigapixel AI, PTGui).

**Python** (open-source путь):
```python
import rawpy, numpy as np
from skimage.registration import phase_cross_correlation
from scipy.ndimage import shift as ndshift

files = ["IMG_0001.CR2", "IMG_0002.CR2", "IMG_0003.CR2", "IMG_0004.CR2"]
frames = [rawpy.imread(f).postprocess(output_bps=16).astype(np.float32) for f in files]
ref = frames[0]
aligned = [ref]
for f in frames[1:]:
    sh, _, _ = phase_cross_correlation(ref, f, upsample_factor=100)
    aligned.append(ndshift(f, (sh[0], sh[1], 0)))
result = np.mean(aligned, axis=0).astype(np.uint16)
```

## Ограничения

- **Движущиеся объекты** дадут ghosting — только для статичных сцен
- **Мануальные стёкла без чипа**: только режим Manual
- **50D буфер**: ~4 RAW, модуль ждёт записи каждого кадра (ML_SHOOT_WAIT_CARD)
- Pixel shift через фокус-мотор = **небольшое изменение плоскости фокуса**,
  а не чистый сдвиг матрицы. Разница минимальна на средних дистанциях,
  но при макро-съёмке — заметна.

## Статус

Структурно корректен для ML API. Требует тестирования на живой камере
(особенно поведение `lens_mli_to_focus` с разными объективами).
