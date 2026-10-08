# Языки интерфейса

Состояние на 2026-10-06.

| Индекс | Код | Файл | Сообщений | Готово | Кто переводил |
|---|---|---|---|---|---|
| 0 | en | `translations/translation_en.ts` | 1059 | все (пустой перевод = исходный текст) | исходный проект |
| 1 | ru | `translations/translation_ru.ts` | 2384 | все | исходный проект; 1122 недостающих дописаны 2026-10-06 |
| 2 | pl | `translations/translation_pl.ts` | 1112 | все из файла | исходный проект; ~1270 новых строк в файле нет — они идут по-английски |
| 3 | zh | `translations/translation_zh.ts` | 2383 | все | 2026-10-06, машинный перевод (Claude), **носителем не вычитан** |

Китайский — упрощённый (`zh_CN`).

## Как устроено

- `qt_add_translations` в `CMakeLists.txt` прогоняет lrelease и кладёт `translation_<код>.qm` в ресурсы
  `:/translations/`. Список кодов — `availableLanguages` в `src/main.cpp` и `langs_` в
  `src/language_controller.h`; выбор языка — вкладки в `qml/app/AppSettingsPage.qml` и на экране
  первого запуска в `qml/app/MainWindow.qml`.
- В настройках хранится **индекс** (`main/appLanguage`), не код. Новый язык — только в конец списка,
  иначе у всех пользователей язык сменится сам.
- При первом запуске берётся язык системы, если он есть в списке (`zh_TW` тоже даёт упрощённый `zh`).
- Старый выбор языка в `qml/legacy_qml/settings/DisplaySettings.qml` нигде не создаётся; его не трогали.

## Как обновить после правок кода

```
D:\Qt\6.8.3\llvm-mingw_64\bin\lupdate.exe src qml -ts translations\translation_ru.ts -no-obsolete
D:\Qt\6.8.3\llvm-mingw_64\bin\lupdate.exe src qml -ts translations\translation_zh.ts -no-obsolete -target-language zh_CN
```

Новые строки появятся с `type="unfinished"`. Их нельзя оставлять: **lrelease кладёт незавершённый
перевод в .qm**, а lupdate заполняет его догадкой из другого контекста с тем же текстом. Так в панели
USBL « m» (минуты) получило « м» (метры). Проверка:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ts_check\ts_check.ps1 ru zh
```

Без языков — все `translation_*.ts`; `-Path файл.ts` — любой файл. Код возврата 0 — чисто, 1 — найдены
ошибки, 2 — нет файла. Проверка ловит незавершённые сообщения, расхождение заполнителей (`%1`, `%L1`,
`%n`), HTML-тегов, числа переносов строк и пробелов по краям — то, что ломает строку при работе или на
экране. Проверено на испорченных копиях `translation_zh.ts`: все четыре вида порчи пойманы.
**Качество перевода она не проверяет — это может только читатель языка.**

При сборке lrelease пишет итог, например `Generated 2383 translation(s) (2383 finished and 0 unfinished)`.

## Одно слово — разные значения

Перевод хранится на пару (исходный текст, комментарий), поэтому одинаковое английское слово в разных
окнах получает один перевод. Где значение другое, перевод задан в своём контексте:

| Текст | Контекст | zh | ru |
|---|---|---|---|
| Channels | DeviceSettingsPage (радиоканалы) | 信道 | Каналы |
| Line | DeviceSettingsPage (линия UART) | 线路 | Линия |
| Points | IsobathsPipelineStatus (число точек) | 点数 | Точки |
| ` m` | UsblNodesPopup (минуты) | ` 分钟` | ` мин` |
| m | DeviceSettingsPage (минуты в аптайме) | 分 | м (из исходного проекта) |
| Open | DeviceSettingsPage (защита Wi-Fi) | 开放 | Открытая |
| Scan | DeviceSettingsPage (поиск сетей) | 扫描 | Сканировать |
| Default | DeviceSettingsPage (заводские адреса) | 默认 | По умолчанию |
| Position | MosaicPipelineStatus | 位置 | Позиция |

## Шрифт для китайского

На русской Windows 11 Qt рисовал китайский текст японским MS UI Gothic, а упрощённые знаки, которых
в нём нет (缩, 键, 击…), — шрифтом SimSun: в одной строке два начертания и японская форма знаков.
`preferSimplifiedChineseFont()` в `src/main.cpp` ставит первым запасным шрифтом для иероглифов
Microsoft YaHei UI (Windows) или Noto Sans CJK SC (Android, Linux). Измерено по глифам `QTextLayout`
на трёх строках интерфейса: до — MS UI Gothic 45 знаков + SimSun 17, после — Microsoft YaHei UI 62.

Не проверено:
- Android-телефон с китайским интерфейсом приложения (есть ли на нём Noto Sans CJK SC).
- Киоск HeadUnit (Linux): без пакета шрифтов CJK (например `fonts-noto-cjk`) иероглифы, скорее всего,
  будут квадратами — предположение, на образе не смотрели.
- 3D-сцена рисует подписи своим атласом на Roboto, в нём иероглифов нет. Переводимых строк там нет,
  но названия контактов, введённые по-китайски, в 3D, вероятно, не отрисуются — вывод из кода, не
  проверено.

## Словарь для вычитки

Основные термины, которых держался перевод. Носителю китайского стоит начать с них.

| English | 中文 | Русский |
|---|---|---|
| Echogram | 回波图 | Эхограмма |
| Bottom track | 底部跟踪 | Трек дна |
| Side-scan mosaic | 侧扫镶嵌图 | Мозаика бокового обзора |
| Isobaths | 等深线 | Изобаты |
| Surface (3D) | 曲面 | Поверхность |
| Sonar | 声呐 | Сонар |
| Recorder | 记录仪 | Рекордер |
| Contact (mark) | 目标 | Контакт |
| Gain / TGC | 增益 / 时间增益补偿（TGC） | Усиление / TGC |
| Firmware / Flash | 固件 / 刷写 | Прошивка / Прошить |
| Arm / Disarm | 解锁 / 上锁 | Взвести / Снять со взвода |
| Mission / Waypoint | 任务 / 航点 | Миссия / Путевая точка |
| Heading / Yaw | 航向 / 偏航 | Курс |
| Epoch | 历元 | Эпоха |
| Tile | 瓦片 | Тайл |
| Pane / Layout / Widget | 窗格 / 布局 / 小部件 | Панель / Компоновка / Виджет |
| Transponder / Interrogation | 应答器 / 询问 | Транспондер / Опрос |
| Beacon / Pinger | 信标 / 声脉冲器 | Маяк / Пингер |
| Calibration stand | 校准测试台 | Калибровочный стенд |
| Servo | 舵机 | Серво |
| Pipeline (diagnostics) | 处理流程 | Конвейер |

Единицы в китайском оставлены латиницей (m, mm, m/s, kHz, dBm), кроме времени (秒, 分钟, 小时).
