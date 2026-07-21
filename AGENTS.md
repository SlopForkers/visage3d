# Character Editor

Редактор персонажей для glTF/VRM моделей (тестовая — `female_base.vrm`, VRoid Studio).
C++17, OpenGL 3.3 core, Dear ImGui. Windows / MinGW.

## Сборка и запуск

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
.\build\character_editor.exe                      # интерактивный режим (из корня репо!)
.\build\character_editor.exe --frames 30 --screenshot shot.png   # пакетный режим
```

Запускать нужно из корня репозитория: пути `female_base.vrm`, `config/`, `presets/` относительные.

CLI: `[model] [--model p] [--preset name] [--screenshot out.png] [--frames N] [--size WxH]`
`[--set id=value]... [--yaw deg] [--dist m] [--targety m] [--clothe path]... [--listparams]`.
Файлы .vrm/.glb/.gltf можно перетаскивать в окно; кнопки «Открыть модель...» / «Добавить одежду...»
открывают стандартный диалог проводника (GetOpenFileName, у одежды — multiselect).

## Архитектура

- `src/core/` — `Math3D.h` (vec/quat/mat, column-major), `GL.{h,cpp}` (свой загрузчик GL-функций через `glfwGetProcAddress`, без glad/glew).
- `src/model/` — `Model.h` (рендер-независимые данные), `GltfLoader` (tinygltf; .vrm/.glb определяются по магии `glTF`), `Skeleton` (мировые матрицы узлов, per-node scale+translate offsets).
- `src/editor/` — `ShapeController` (параметры тела/лица/морфов), `Presets` (JSON в `presets/`).
- `src/clothing/` — `ClothingManager`: автоподгонка и автоскиннинг одежды (см. ниже).
- `src/render/` — `Shader`, `Camera` (орбитальная), `ModelRenderer` (мульти-модель по слотам: слот 0 = тело, остальные = одежда; GPU-скиннинг до 80 костей; 2 прохода opaque/mask → blend; sRGB; флаги волос `hideHair`/`hairTint`).
- `src/ui/` — `EditorUI` (левая панель 340px, вкладки сверху как в VRoid: **Тело | Лицо | Причёска | Одежда | Пресеты | Вид**, русский UI, кириллица из segoeui/arial).
- `src/app/` — `Application` (окно GLFW, цикл, drag&drop, скриншоты, debounced авто-подгонка), `FileDialog` (Win32 GetOpenFileNameW).

## Параметры тела и лица

Правила в `config/body_params.json` (точные имена + regex-паттерны для других моделей);
без файла — встроенные правила. Значение нормировано 0..1, масштаб = lerp(min,max),
по осям — `pow(s, axisExp)`. Два формата правил:
- плоский (`bones/patterns/axes/compensate` на верхнем уровне);
- `entries`: несколько правил на параметр, у каждого свои axes; `translate` — смещение кости
  (`mode: value` → `axis*factor*(v-def)`, `mode: scale` → `axis*factor*(s-1)`).

Тело: `breast_size`, `buttocks_size`, `hip_width`, `waist_size`, `weight` (hips+spine+chest+бедра+плечи),
`belly_abs`, `height` (ноги+спина по Y + подъём hips на `0.78*(s-1)`, чтобы ступни оставались на полу),
`leg_length`, `arm_length`. Лицо: `head_size`, `eye_size`, `eye_spacing`, `eye_height`
(последние два — translate по `J_Adj_*_FaceEye`). Компенсация (`compensate:true`) — дочерним костям
обратный масштаб (ягодицы не растягивают торс).

## Одежда (статические меши без скелета)

Пайплайн (`ClothingManager`):
1. Загрузка + запечка node-трансформов в вершины.
2. **Авто-единицы**: диагональ bbox → кандидаты {100..0.0001}, правдоподобие 0.25..2.3 м
   (mm/cm/dm Sketchfab-модели).
3. **Авто-подгонка**: центровка по X/Z + координатный спуск (масштаб ±, смещение X/Z) по метрике
   «среднее расстояние до тела с кепом 0.25 м + штраф-инерция за отход от авторской посадки»
   (Y не трогаем — одежда выровнена по земле при авторстве; масштаб относительно «пивота» —
   низа bbox, чтобы не уезжало вверх/вниз).
4. **Перенос весов**: облако точек тела (текущая поза, скиннинг на CPU, с нормалями) +
   spatial hash (ячейка 5 см, кольца 1-2, дальше — brute force, многопоточность по примитивам).
   K=4 ближайших → веса с inverse-square falloff → топ-4 кости на вершину.
5. **Клэмп наружу**: вершины внутри тела/ближе `padding` выталкиваются на `target + нормаль_тела * padding`
   (знаковое расстояние вдоль нормали). `shrink` (0..1) — обтяжка к поверхности.
6. **Авто-подгонка при смене фигуры**: `ShapeController.revision` → таймер 0.75 с → refit всей одежды
   (чекбокс «Авто-подгонка под фигуру»). Между рефитами одежда деформируется костями как тело.

UI одежды: видимость, Отступ (мм), Обтяжка, Масштаб, Смещение, «Подогнать под тело», «Удалить».

## Ключевые решения / подводные камни

- **JOINTS_0 у тела VRoid — float32** (не u16/u8) — читаем и такое.
- **mat3→quat для узлов с `matrix`** — следить за column-major индексами (Shepperd).
- Динамический VBO (pos+nrm) заполняется для ВСЕХ примитивов, не только морфовых.
- Причёска: цвет/видимость (материалы с `hair` в имени) на вкладке «Причёска»;
  своя причёска = грузится как предмет одежды (веса лягут на голову).
- Ограничения: экстремальные формы (грудь ×1.8) могут не покрываться чужой одеждой
  (ткани физически не хватает) — решается слайдерами «Масштаб/Обтяжка/Отступ»;
  A-поза рукавов у источника vs T-поза тела — искажение рукавов при анимации;
  `COLOR_0`, spring bones, MToon shade/rim/outline не поддерживаются.
- Пресеты v2: `{"model", "values": {id: 0..1}, "clothing": [{path, fitScale, fitOffset, padding, visible}]}`.

## Ограничения / TODO

- Макс. 80 костей в uniform-массиве.
- Экспорт обратно в .vrm не реализован (только пресеты JSON).
- UI только русский; без шрифта с кириллицей подписи сломаются.
- `models/` — исходники одежды (zip распакованы в одноимённые подпапки).
