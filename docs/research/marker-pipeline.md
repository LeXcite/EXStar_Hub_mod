# Протокол сигнала «красный маркер» — EXStar Hub 3.1.49

Описание механизма стандартной программы: что означает красный цвет маркера, кто его
ставит, по какому триггеру и с какой динамикой. Раздел 8 — проверенный способ
перехвата и подмены этого сигнала извне.

---

## 1. Суть сигнала

Красный диск маркера — это **не «цвет маркера» и не состояние его машины состояний**, а
**цвет 3D-модели-группы с именем `CURRENT_MARKER`**. Каждый кадр сканирования алгоритм
сообщает, коды каких маркеров он увидел именно в этом кадре; сцена перекладывает эти узлы
маркеров в группу `CURRENT_MARKER`, для которой uniform-ы маркерного шейдера:

- `frontInnerColor = (1, 0, 0, 1)` — красный диск,
- `frontExternalColor = (1, 0, 0, 0.5)` — красное полупрозрачное кольцо.

Все остальные маркеры остаются в «белой» группе: `frontInnerColor = (1, 1, 1, 1)`.
OSG переприменяет все uniform-ы каждый кадр, поэтому красный «живёт», пока маркер
попадает в каждый новый кадр, и снимается на ближайшей же операции кадра, где маркера
уже нет.

---

## 2. Цепочка триггера

```
[L0] Алгоритм (регистрация/распознавание кодов пластин маркеров по кадру)
     Sn3DRegistration.dll: MarkerPtCloud, RangeDataDense::set_markerPts —
     «какие маркеры и где обнаружены в текущем кадре»
        │  Qt-сигналы
        ▼
[L1] Sn3DScanController.dll
     сигналы generatePointCloudFinished / registrationFrameMarkerFinished,
     слот onRegistrationFrameMarkerFinished, публикация generateFrameMarker
     (строки: "publish generateFrameMarker start", "generateFrameMarker.sync")
        │  очередь событий Qt (posted event → GUI-поток)
        ▼
[L2] Конвейер пост-обработки: PostProcess.dll / PostProcessUI.dll
     шаг конвейера с именем "PostProcess.FrameMarker"
     (PostProcessUI FUN_18000e750, RVA 0xE750): сверяет имя шага со строкой
     "PostProcess.FrameMarker", строит osg::TemplateArray<osg::Vec3f,28,3,5126>
     — массив точек маркеров из данных алгоритма — и передаёт операцию
     через SceneControl / SceneControlProxy
        │  вызов в плагин сцены
        ▼
[L3] sn3d.digit.scenecontrol.dll  (Qt-плагин канваса, экспортирует qt_plugin_instance)
     диспетчер сценовых операций FUN_18004f9f0;
     обработчик "PostProcess.FrameMarker" FUN_18005f5c0 читает payload:
     uuid, markerFids, markerLastFids, markerVids, markerUuid
     (строки на RVA 0x197860..0x197890);
     перекладка узлов в модель "CURRENT_MARKER";
     FUN_180024390: сравнение имени модели с UTF-16 "CURRENT_MARKER" (14 симв.) →
        ДА : frontInnerColor = (1,0,0,1), frontExternalColor = (1,0,0,0.5)
        НЕТ: frontInnerColor = (1,1,1,1)
     там же развилка по размеру маркера → пары радиусов 3/1.5, 5/3, 9/6
     (65-байтная запись pos/normal/diameter)
        │  экспорт Sn3D::RenderKit::ExtDigit::ModelNode::setMarkerPoints(Sn3DMarkerParam)
        ▼
[L4] librenderkit.extensions.digit.dll  (владелец шейдеров/uniform-ов маркеров)
     setMarkerPoints (RVA +0xAD195, +0xAD52C внутри экспорта) — запись vec4
     в osg::Uniform "frontInnerColor"/"frontExternalColor"
     через osg::Uniform::set / setElement;
     librenderkit+0x14C447 (зона createMarkerPoint@MarkerPoint) — геометрия маркера
        │  osg::Uniform::set(Vec4f)/setElement(uint,Vec4f) [экспорт osg158-osg.dll]
        ▼
[L5] OSG → OpenGL
     StateSet маркерной программы; на render-потоке osg::OperationQueue::runOperations
     (osg+0x11670B) и обход графа osgViewer пере-выставляют uniform каждый кадр →
     glUniform4fv(location=1, frontInnerColor)
```

---

## 3. Стек выполнения (натурный, из запущенной программы)

Запись значения красного (GUI-поток, Qt-событие операции кадра),
через `osg::Uniform::setElement(Vec4f)`:

```
osg158-osg.dll+0x16E0EF                          (Uniform::set… internal)
librenderkit.dll+0x14C447                        (зона createMarkerPoint@MarkerPoint)
librenderkit.extensions.digit.dll+0x1329BA
librenderkit.extensions.digit.dll+0x13684E
librenderkit.extensions.digit.dll+0xAD195        (setMarkerPoints@ModelNode +0x355)
sn3d.digit.scenecontrol.dll+0x52AA9              (исполнитель сценовой операции)
sn3d.digit.scenecontrol.dll+0x35DB1              (FUN_180035bc0+0x1F1 — вызов операции)
sn3d.digit.scenecontrol.dll+0x33D9F              (FUN_180033d00 — Qt-обработчик события)
Qt5Core.dll+0x2052B3                             (QObject::event+0x183)
Qt5Widgets.dll+0x17A1A                           (QApplicationPrivate::notify_helper+0x13A)
```

Вариант со вторым исполнителем: `ext.digit+0xAD52C (setMarkerPoints+0x6EC) →
scenecontrol+0x518F8 → те же 0x35DB1 / 0x33D9F → Qt`.

Применение каждый кадр (render-поток QtQuick/osgViewer),
через `osg::Uniform::set(Vec4f)`:

```
librenderkit.extensions.digit.dll+0x12F1DF
osg158-osg.dll+0x11670B                          (OperationQueue::runOperations+0xBB)
osg158-osgViewer.dll+0x16E97 / +0x49357          (кадр osgViewer)
librenderkit.dll+0xFA2D2
Qt5Quick.dll+0x1F6B7E → Qt5Core.dll+0x1FD37D (qt_static_metacall) → Qt5Quick render loop
```

Перехваченные записи значения:

```
SetElement idx=0 RED   v=(1.00,0.00,0.00,1.00) name=frontInnerColor
Set4       idx=—  RED   v=(1.00,0.00,0.00,1.00) name=frontInnerColor
```

---

## 4. Тайминг и динамика

Сессия «тарелка в кадр / из кадра» (4 внесения), красные записи `frontInnerColor`:

| # | длительность в кадре | красных записей |
|---|----------------------|-----------------|
| 1 | 30.4 s | 7044 |
| 2 | 4.8 s  | 1464 |
| 3 | 5.3 s  | 1230 |
| 4 | 4.0 s  | 752  |

* Частота внутри вспышки ≈ **230 красных записей/сек** — каждый кадр рендера
  (~30–60 fps) × каждый маркер в кадре (set + setElement, несколько маркеров).
* Красный появляется на первой же операции кадра, где коды тарелки распознаны.
* Красный снимается **на ближайшей же операции кадра, где маркера нет** (≤1 кадра
  алгоритма; паузы между внесениями 1.5–2.8 с — в это время только белый).
* Отношение за сессию: RED ≈ 10 490 (set) / 19 031 (setElement) против
  WHITE ≈ 60 419 / 61 810 — белые пишутся постоянно, для всех маркеров не в кадре.
* Сброс по завершении скана: отдельный путь `cleanCurMarker` / `cleanCurCloud`
  (SendDataToClientController.dll: «ready to send cleanCurCloud message to
  scenecontroller») — при остановке/очистке красная группа снимается,
  маркеры становятся белыми.

---

## 5. Семантика цвета

| `frontInnerColor` | Смысл |
|---|---|
| `(1,0,0,1)` красный диск (+ кольцо `(1,0,0,0.5)`) | маркер распознан **в текущем (последнем) кадре** сканера; узел лежит в модели `CURRENT_MARKER`; переподтверждается каждый кадр |
| `(1,1,1,1)` белый | любой другой маркер: зарегистрированный ранее, сейчас вне кадра, или удалённый из `CURRENT_MARKER` очередной операцией кадра |

---

## 6. Проверенные исключения (что НЕ является триггером)

* `setState@MarkerNodeBase`, `setStateParams@MarkerNodeBase`, `updateMarker@Sn3DMarker`
  (экспорты librenderkit.extensions.digit.dll) — **0 вызовов** за реальную сессию
  скана. Машина состояний узлов (`enum State`, `QMap<State,Param>`) к покраске
  «в кадре» не причастна.
* Вершинные цвета, красные текстуры, иные uniform-ы — не участвуют: красный приходит
  только через `glUniform4fv` (loc=1 диск, loc=0 кольцо) маркерной программы.

---

## 7. Версионно-устойчивые якоря (для переноса на другие сборки)

Не использовать RVA. Якоря:

1. Строка **`frontInnerColor`** (и опечатка вендора `innerDiameterTheshold`) — маркерный
   шейдер; uniform-ы маркерной программы: u0 `frontExternalColor` (кольцо), u1
   `frontInnerColor` (диск), u3 `normal`, u4 `osg_ModelViewMatrix`,
   u5 `osg_ModelViewProjectionMatrix`, u8 `radius` — ОДИНАРНЫЙ FLOAT (float, loc 8;
   живое измерение через glGetProgramiv/ACTIVE_UNIFORMS, type=0x8B52 — более ранняя
   запись «vec2» была ошибочной).
2. Экспорты OSG: `?set@Uniform@osg@@QEAA_NAEBVVec4f@2@@Z`,
   `?setElement@Uniform@osg@@QEAA_NIAEBVVec4f@2@@Z` (имя uniform-а читается из inline
   `std::string` объекта `osg::Uniform`).
3. UTF-16 строка **`CURRENT_MARKER`** в `sn3d.digit.scenecontrol.dll` (+ ASCII
   `markerFids/markerLastFids/markerVids/markerUuid`) — место решения о покраске.
4. Строка операции **`PostProcess.FrameMarker`** в `PostProcess.dll`/`PostProcessUI.dll`
   (шаг конвейера) и в `sn3d.digit.scenecontrol.dll` (обработчик).
5. Сигналы/строки `registrationFrameMarkerFinished`, `generateFrameMarker` в
   `Sn3DScanController.dll` — кадровый источник.
6. Экспорт `?setMarkerPoints@ModelNode@ExtDigit@RenderKit@Sn3D@@QEAAIUSn3DMarkerParam@234@@Z` —
   точка применения цвета к узлам.

---

## 8. Способ перехвата и изменения сигнала (проверенный)

Цель: подменить сигнал «маркер в кадре» (§1), не патча чужие DLL на диске.
Реализация — трёхслойный фреймворк этого репозитория:

```
winmm.dll                          bootstrap-прокси   (src/winmm_proxy.c)
  └─ ExStarRec.dll                 хост-менеджер      (src/host.c)
       └─ modules\marker_inframe.dll   модуль поведения (modules/marker_inframe/)
```

### 8.1 Точка входа — прокси winmm.dll

- `EXStar Hub.exe` сам winmm не импортирует, но импортирует `Qt5Core.dll → WINMM.dll`;
  на Win11 winmm отсутствует в KnownDLLs ⇒ загрузчик берёт `winmm.dll` из каталога
  приложения.
- Прокси форвардит все ~180 именованных экспортов в `winmm_real.dll` (копия системной
  winmm; генерация форвардеров — `tools/gen_forwards.ps1`; один безымянный ordinal не
  проброшен — приложением не используется) и грузит хост `ExStarRec.dll` из того же
  каталога.
- `DllMain` прокси и хоста всегда возвращают TRUE; вся работа — в worker-потоке хоста.

### 8.2 Точка перехвата — glUniform4fv

- Хост создаёт собственное скрытое GL-окно с контекстом и ждёт инициализации GL
  приложением; модуль получает драйверный thunk `wglGetProcAddress("glUniform4fv")`
  через `host->gl_thunk` (thunk общепроцессный и одинаков между запусками — адрес
  НЕ хардкодится).
- Собственный `opengl32.dll` в каталог класть бессмысленно: KnownDLL, грузится из
  system32 (проверено).
- `osg::Uniform::set/setElement(Vec4f)` тоже подходят (экспорты osg158-osg.dll), но
  OSG пере-выставляет uniform каждый кадр (§2, L5) ⇒ перехвата upload достаточно.
- Detour ставится MinHook'ом через обёртку хоста (`host->hook`); тело detour — под SEH;
  никакого хардкода адресов/RVA — только импорт-имена и строки.

### 8.3 Фильтр «это красный сигнал маркера» (все три условия)

1. `location == 1 && 1 <= count <= 64` (фиксированная локация диска, u1);
2. значение красное: `v0 >= 0.55 && v1 <= 0.38 && v2 <= 0.38` (§5);
3. текущая программа — маркерная: `glGetUniformLocation(prog,"frontInnerColor") == 1`
   (кеш по id программы; кольцо — u0 `frontExternalColor`, радиус — u8 `radius`).

Живая проверка: за сессию 3341/3341 красных записей прошли фильтр (0 ложных
срабатываний) ⇒ динамический fallback не нужен.

### 8.4 Подмена и конфигурация

- Подменяется только RGB красных элементов, alpha вендора сохраняется.
  Живое уточнение: вендор перекрашивает только диск (u1); кольцо (u0) красным
  не выставляется никогда (в логе — только `RED inner`), поэтому у кольца
  подмена цвета фактически не срабатывает и чёрная кайма остаётся чёрной.
- Увеличение «в кадре» НЕ делается в upload-хуке: OSG выставляет uniform только
  при смене значения, `radius` — общий для всех маркеров, и его правка на
  upload «протекла» бы на все последующие отрисовки. Размер собирается на
  границе одного draw call (`glDrawArrays`/`glDrawElements` маркерной программы,
  флаг «последняя загрузка диска была красной»): на время реального вызова
  `radius` умножается на `scale`, у `osg_ModelViewMatrix` и
  `osg_ModelViewProjectionMatrix` масштабируются 3×3 части (умножение справа —
  колонка трансляции не трогается, маркер остаётся на месте), а `radius` +
  `frontExternalColor` перекрашиваются в `outline_color`; сразу после draw всё
  возвращается. Ни один чужой draw не видит изменённых значений.
- Таким образом маркер «в кадре» увеличивается в `scale` раз и получает цветную
  (по умолчанию зелёную) окантовку вместо чёрной; вне кадра — исходные размер
  и кайма.
- Включение/выключение модуля: `<app>\modules.ini`, секция `[marker_inframe]
  enabled = 0|1` (применяется при старте приложения; выключенный модуль не грузится).
- Настройки: `<app>\modules\marker_inframe.ini` → `scale = F` (1.0..4.0,
  по умолчанию 1.6), `outline_color = R G B` (цвет каймы увеличенного маркера,
  по умолчанию `0 1 0`), `color_inner`/`color_ring = R G B` (float 0..1),
  `probe = 0|1` (диагностика: uniform-ы и исходник шейдера маркерной программы,
  события grow/outline). В GUI менеджера `scale` доступен слайдером (1.0..2.0),
  в CLI — ключом `--scale`; оба пишут значение в этот ini (применяется со
  следующего старта приложения).
- Диагностика: `[host] log = 1` в modules.ini →
  `%LOCALAPPDATA%\ExStarHubMods\mod.log` (`armed`, счётчики `calls/recolored`).
- Полный откат: удалить из каталога приложения `winmm.dll`, `winmm_real.dll`,
  `ExStarRec.dll`, `modules.ini` и каталог `modules\`.

### 8.5 Правила и тупики

- Чужие DLL не патчатся на диске: только прокси + inline-хуки в памяти.
- Проверенные тупики (не повторять): экспорты `setState/setStateParams/updateMarker`
  не вызываются (§6); вершинные цвета и текстуры красный не несут; кастомный
  `opengl32.dll` не загружается.
- Приложение при старте кратко дублирует процесс (второй экземпляр выходит через
  ~3 с) — норма, хуки взводятся в обоих.
