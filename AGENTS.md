# Character Editor

Редактор персонажей для glTF/VRM моделей (тестовая — `female_base.vrm`, VRoid Studio).
C++17, OpenGL 3.3 core, Dear ImGui. Windows / MinGW.

## Сборка и запуск

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
.\build\character_editor.exe                      # интерактивный режим (из корня репо!)
.\build\character_editor.exe --frames 30 --screenshot shot.png   # пакетный режим
```

Запускать нужно из корня репозитория: пути `female_base.vrm`, `config/`, `presets/` относительные.

CLI: `[model] [--model p] [--preset name] [--screenshot out.png] [--frames N] [--size WxH]`
`[--set id=value]... [--yaw deg] [--dist m] [--targety m]` — камера/переопределение параметров (для тестов).
Файлы .vrm/.glb/.gltf можно перетаскивать в окно.

## Архитектура

- `src/core/` — `Math3D.h` (vec/quat/mat, column-major), `GL.{h,cpp}` (свой загрузчик GL-функций через `glfwGetProcAddress`, без glad/glew).
- `src/model/` — `Model.h` (рендер-независимые данные), `GltfLoader` (tinygltf; .vrm/.glb определяются по магии `glTF`, не по расширению), `Skeleton` (мировые матрицы узлов, костные матрицы, per-node scale offset).
- `src/editor/` — `ShapeController` (параметры тела/морфов), `Presets` (JSON в `presets/` через nlohmann json из tinygltf).
- `src/render/` — `Shader`, `Camera` (орбитальная), `ModelRenderer` (GPU-скиннинг до 80 костей, CPU-морфинг в динамический VBO, 2 прохода: opaque/mask → blend, sRGB-текстуры + гамма в шейдере).
- `src/ui/` — `EditorUI` (левая панель 340px, русский UI, шрифт segoeui/arial с кириллицей).
- `src/app/` — `Application` (окно GLFW, цикл, drag&drop, скриншоты через stb_image_write; реализация stb — в `GltfLoader.cpp`).

## Ключевые решения

- **Параметры тела** (`breast_size`, `buttocks_size`, `hip_width`, `waist_size`) — масштаб костей. Правила в `config/body_params.json` (точные имена + regex-паттерны, чтобы работало с разными моделями); без файла — встроенные правила в `ShapeController.cpp`. Значение нормировано 0..1, масштаб = lerp(min,max), по осям — `pow(s, axisExp)`.
- **Компенсация**: при `compensate:true` дочерним костям задаётся обратный масштаб (ягодицы не растягивают торс/ноги). У груди компенсация выключена: `Bust2` наследует масштаб `Bust1` — сосок уезжает вперёд естественно.
- **Морфы** — CPU-блендинг только активных таргетов (вес ≠ 0) → `glBufferSubData` в динамический VBO при `morphsDirty`. У female_base морфы только у лица (57 таргетов, группы `Fcl_<BRW|EYE|MTH|HA|ALL>_*`).
- **Нормали** при скиннинге с неравномерным масштабом — inverse-transpose в вершинном шейдере (`transpose(inverse(mat3(skin)))`), массив нормальных матриц не передаётся.
- **Материалы**: MToon/unlit рендерятся упрощённо (half-lambert + spec), `KHR_texture_transform` игнорируется (у VRoid он identity), `alphaMode` OPAQUE/MASK/BLEND поддержан, doubleSided → отключение culling + flip нормали по `gl_FrontFacing`.
- `COLOR_0`, spring bones, MToon shade/rim/outline не поддерживаются (не нужны для редактора формы).
- Пресеты хранят только отличные от дефолта значения: `{"model": ..., "values": {id: 0..1}}`.

## Ограничения / TODO

- Макс. 80 костей в uniform-массиве (лишние обрезаются).
- Экспорт обратно в .vrm не реализован (только пресеты JSON).
- UI только русский; без шрифта с кириллицей подписи сломаются.
