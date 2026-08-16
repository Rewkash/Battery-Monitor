# Обзор приложения ChargeView

## Назначение и аудитория

**ChargeView** — нативное настольное и консольное приложение для просмотра заряда Bluetooth-устройств. В исходном коде, именах исполняемых файлов и CMake-целей также используется историческое имя **Battery Monitor**.

Приложение предназначено прежде всего для пользователей Windows 10/11 и Linux/BlueZ, которым требуется:

- видеть заряд Bluetooth-периферии в одном месте;
- различать общий заряд устройства и компоненты `left`, `right`, `case`, `main`;
- видеть, является ли значение актуальным или кэшированным;
- получать предупреждения о низком заряде;
- хранить локальную историю и оценивать оставшееся время работы;
- управлять шумоподавлением поддерживаемых Xiaomi/Redmi TWS-устройств;
- получать машиночитаемый JSON из CLI.

Это не универсальный Bluetooth-драйвер. Наличие показаний и команд управления зависит от ОС, доступных системных API, конкретного устройства и реализованного vendor-протокола.

## Технологический стек

| Область | Технологии |
|---|---|
| Основной код | C++20, стандартная библиотека |
| Сборка | CMake 3.21+, MSVC на Windows, GCC/Clang на Linux |
| GUI | Qt 6 Widgets, Svg, Network |
| Windows Bluetooth | C++/WinRT, Bluetooth LE/GATT, AEP/PnP, RFCOMM и Windows device properties |
| Linux Bluetooth | BlueZ через D-Bus (`libdbus-1`) |
| Криптография обновлений | Monocypher/Ed25519, SHA-256 |
| Упаковка Windows | MSI через Python `msilib`, ZIP и собственный `.bmup` bundle |
| Release tooling | Python, PowerShell, GitHub Actions |
| Конфигурация устройств | JSON-профили в `profiles/devices` |
| Локальное состояние GUI | INI через `QSettings`; JSON-файлы истории и состояния updater |

Серверной части, собственной БД, очередей сообщений и публичного HTTP API в проекте нет. Единственная внешняя сетевая интеграция приложения — Windows updater, который загружает подписанный манифест и release-артефакты из GitHub Releases.

## Архитектура

### Общая схема

```text
GUI / CLI entry point
        |
        v
BatteryMonitorMain + CLI parser/formatter
        |
        v
IBluetoothBatteryProvider <---- DeviceProfiles
        |
        +-- Windows: WinRtBatteryProvider
        |      +-- BLE/AEP/PnP/standard battery readers
        |      +-- phone/controller/device-specific readers
        |      +-- Xiaomi classic/BLE sessions and codecs
        |      `-- INoiseControlProvider
        |
        `-- Linux: BluezBatteryProvider -> system D-Bus -> BlueZ

Qt GUI -> history/statistics/runtime estimator/diagnostics
Qt GUI or CLI --check-updates -> UpdateService -> signed GitHub assets
                                      |
                                      `-> maintenance helper -> MSI or portable replacement/rollback
```

### Общий доменный слой

Публичные типы в `include/core` отделяют приложение от платформенной реализации:

- `DeviceBatteryInfo` — одно показание батареи компонента устройства;
- `BatteryQueryOptions` — режим запроса, включая offline-устройства, live refresh и выбор устройства;
- `IBluetoothBatteryProvider` — получение показаний батареи;
- `INoiseControlProvider` — проверка и изменение ANC/transparency/off и подрежимов;
- `DeviceProfile` и связанные типы — правила классификации и метаданные устройств;
- `ProviderFactory` — создание реализации для текущей ОС.

### Windows provider

`WinRtBatteryProvider` координирует несколько источников и объединяет результаты:

- WinRT/AEP enumeration Bluetooth-устройств;
- стандартный BLE Battery Service;
- Windows PnP/device-property hints;
- специализированные readers для телефонов, контроллеров и ряда Bluetooth-классов;
- Xiaomi/Redmi/ZMI classic RFCOMM и BLE vendor-протоколы;
- persistent cache для показаний, которые нельзя получить live;
- шумоподавление через тот же provider.

Подсистема Xiaomi разделена на поиск endpoint, транспорт, сессии, auth/codec, battery/noise-control protocol и persistent cache. Часть поведения определяется C++-эвристиками, часть — JSON-профилями.

### Linux provider

`BluezBatteryProvider` синхронно обращается к system D-Bus, получает объекты BlueZ через `ObjectManager.GetManagedObjects`, извлекает `org.bluez.Battery1.Percentage` и сведения `org.bluez.Device1`, затем приводит их к общей модели. Vendor-specific Xiaomi control на Linux не реализован.

### GUI

Qt-приложение построено вокруг `BatteryWindow`. Главное окно:

- запускает фоновые запросы provider;
- принимает Windows Bluetooth events и инициирует targeted refresh;
- строит карточки подключённых и offline-устройств;
- управляет tray, сортировкой, настройками и уведомлениями;
- записывает историю и показывает статистику/ETA;
- предоставляет noise-control UI;
- показывает диагностику provider;
- запускает и отображает обновление.

Запрос provider выполняется в отдельном worker-потоке, результат возвращается в UI thread через queued invocation. Одновременные refresh-запросы сводятся к текущему и pending-запросу.

### Updater

Updater доступен только в Windows Qt-сборке. Он:

1. загружает JSON-манифест и отдельную Ed25519-подпись;
2. проверяет подпись до разбора JSON;
3. проверяет schema, channel, срок действия, sequence, URL, размер и SHA-256 артефакта;
4. выбирает MSI для установленной версии или `.bmup` для portable-версии;
5. передаёт применение обновления отдельному `battery-monitor-maintenance`;
6. использует staging, backup, startup-health handshake и rollback.

Приватный ключ находится только в GitHub Actions secret; в приложение встроен публичный ключ.

## Структура репозитория

| Путь | Назначение |
|---|---|
| `include/core` | Публичные доменные типы и интерфейсы provider/profile/noise control |
| `src/app` | CLI options, форматирование text/JSON, платформенный command dispatcher |
| `src/core` | Загрузка профилей, vocabulary noise control, factory provider |
| `src/platform/windows` | Windows provider, Bluetooth enumeration/readers, Xiaomi transports/protocols/cache, диагностика |
| `src/platform/linux` | BlueZ/D-Bus provider и Linux command dispatcher |
| `src/ui` | Qt Widgets UI, настройки, история, статистика, ETA, диагностика и update dialog |
| `src/update` | Манифест, безопасность, сеть, install mode, startup health и maintenance helper |
| `profiles/devices` | Runtime JSON-профили семейств устройств |
| `assets/icons` | SVG-иконки категорий устройств |
| `resources` | Qt resource collection |
| `cmake` | Шаблоны сгенерированных version/installer headers |
| `scripts/release` | Создание ZIP/`.bmup`/MSI, подпись и проверка release |
| `scripts/signing` | Development Authenticode certificate/signing helpers |
| `.github/workflows` | Windows build и tag-driven release pipeline |
| `docs` | Дизайн updater, release process и этот обзор |

Каталоги `build*`, `out`, локальные `logs`, `.obj` и `.serena` являются локальными артефактами рабочего окружения и не входят в tracked-исходники проекта.

## Основные сущности и связи

### `DeviceBatteryInfo`

Представляет не устройство целиком, а показание одного его компонента:

- `device_id`, `device_name`;
- `battery_component`: обычно `main`, `left`, `right` или `case`;
- optional `battery_level_percent`;
- optional mode/submode;
- BLE appearance и Bluetooth Class of Device;
- вычисленные категории;
- флаги `is_cached`, `is_connected`.

Одному физическому устройству может соответствовать несколько записей с одинаковым `device_id` и разными компонентами.

### Профили устройств

`DeviceProfile` связывает:

- идентификатор и отображаемое имя профиля;
- платформы, vendor и family;
- категории;
- matcher-ы по имени и device ID;
- декларативные battery/noise-control capability.

Сейчас runtime фактически использует профили прежде всего для matching family/category. Поля `reader`, `transport` и `strategy` пока не образуют полностью profile-driven dispatch.

### История

GUI хранит временные ряды по устройству/компоненту. Точка содержит время, заряд и доступные mode/submode. На основе live-точек строятся графики, статистика расхода и оценка оставшегося времени. Это файловое локальное хранилище, не реляционная БД.

### Манифест обновления

`UpdateManifest` содержит schema/version/channel/sequence, release notes, даты публикации и истечения, mandatory-флаг, а также portable и MSI artifacts с URL, размером, форматом и SHA-256.

## Ключевые пользовательские сценарии

### Просмотр батарей в GUI

1. `battery-monitor` создаёт Qt application и single-instance guard.
2. Через factory создаётся платформенный provider.
3. `BatteryWindow` запускает фоновый refresh.
4. Provider перечисляет устройства, читает доступные источники и агрегирует компоненты.
5. Профили и системные свойства уточняют family/category.
6. UI обновляет карточки, tray и offline/live состояние.
7. Live-изменения попадают в историю; при пересечении порога может появиться уведомление.

### Получение данных из CLI

1. `battery-monitor-cli` вызывает общий `BatteryMonitorMain` с предпочтением CLI.
2. Разбираются аргументы.
3. Provider выполняет запрос батарей.
4. Результат печатается как человекочитаемый текст или JSON.
5. Ошибки provider приводят к сообщению в stderr и ненулевому exit code.

### Управление Xiaomi/Redmi noise control

1. Пользователь выбирает режим в GUI или передаёт CLI-команду.
2. Platform dispatcher/`INoiseControlProvider` проверяет поддержку устройства.
3. Windows Xiaomi session выбирает транспорт, при необходимости выполняет auth и отправляет vendor-команду.
4. После команды запускается targeted live refresh для подтверждения состояния.

### История и прогноз

1. GUI записывает отфильтрованные live-замеры.
2. History dialog читает series выбранного компонента.
3. Statistics calculator вычисляет изменение и скорость расхода.
4. Runtime estimator отбрасывает непригодные интервалы и оценивает ETA с уровнем уверенности.

### Автоматическое обновление Windows

1. Проверка запускается из GUI или `--check-updates`.
2. Манифест и подпись проверяются, версия сравнивается с текущей.
3. После согласия пользователя пакет скачивается и проверяется.
4. Maintenance helper применяет MSI upgrade либо portable transaction.
5. Новая версия подтверждает успешный запуск; иначе helper выполняет rollback.

## Точки входа

### Исполняемые файлы

- `src/gui_main.cpp` → `battery-monitor`: GUI-preferred entry point при наличии Qt;
- `src/cli_main.cpp` → `battery-monitor-cli`: CLI entry point в Qt-сборке;
- `src/main.cpp`: общий `BatteryMonitorMain` и CLI/GUI orchestration;
- `src/update/maintenance_main.cpp` → `battery-monitor-maintenance`: внутренний Windows update helper.

Без Qt цель `battery-monitor` использует CLI entry point.

### Пользовательские CLI-флаги

- `--version` — версия;
- `--json` — JSON-вывод;
- `--cli` — принудительно CLI;
- `--gui` — принудительно GUI, если он собран;
- `--all`, `--include-offline` — включить отключённые/кэшированные записи;
- `--check-updates` — проверить обновления в CLI;
- `--xiaomi-set-noise <mode> [device-id]` — изменить noise-control mode;
- `--xiaomi-set-submode <mode> <submode> [device-id]` — изменить подрежим.

У maintenance helper есть внутренние аргументы `--apply`, `--apply-msi`, `--update-health-handle` и `--update-health-version`; они не являются публичным пользовательским API.

### Фоновые задачи

Отдельного cron/service нет. В GUI работают:

- периодический timer обновления батарей;
- Bluetooth watcher и отложенные targeted refresh;
- worker-поток текущего provider query;
- timers updater/health-handshake;
- tray и low-battery notifications.

### HTTP/API

Приложение не слушает HTTP-порт и не предоставляет API endpoints. Updater является HTTP-клиентом фиксированных HTTPS URL GitHub Releases.

## Конфигурация и переменные окружения

### CMake options

- `BATTERY_MONITOR_ENABLE_QT` — попытаться собрать Qt GUI; при отсутствии Qt текущая сборка молча становится CLI-only;
- `BATTERY_MONITOR_ENABLE_UPDATER` — включить Windows updater при наличии Qt.

Версия приложения и MSI UpgradeCode задаются в корневом `CMakeLists.txt` и попадают в generated headers.

### Runtime environment

- `BATTERY_MONITOR_PROFILE_DIR` — явный каталог JSON-профилей устройств.

Если переменная не задана, поиск идёт от `profiles/devices` в текущем каталоге и его родителях, а Windows-сборка также учитывает расположение executable.

### GUI settings

В user-scope INI через `QSettings` сохраняются:

- порядок connected/disconnected устройств;
- интервал refresh;
- порог низкого заряда;
- интервал повторных уведомлений.

История, updater state и Windows diagnostic/cache-файлы хранятся в пользовательских локальных каталогах приложения.

### CI/release secrets

- `BATTERY_MONITOR_ED25519_PRIVATE_KEY_B64` — seed подписи update manifest;
- `WINDOWS_SIGNING_PFX_BASE64` — production Authenticode certificate;
- `WINDOWS_SIGNING_PFX_PASSWORD` — пароль production PFX;
- `WINDOWS_TEST_SIGNING_PFX_BASE64` — test certificate для test releases;
- `WINDOWS_TEST_SIGNING_PFX_PASSWORD` — пароль test PFX.

Значения секретов не должны присутствовать в репозитории или логах.

## Технический долг и ограничения

- В репозитории отсутствуют tracked unit/integration tests и CTest-конфигурация; CI выполняет только Windows build и `--version` smoke test.
- Linux build не проверяется CI.
- `BatteryWindow.cpp` концентрирует UI, orchestration, refresh, notifications, history, diagnostics и updater integration и превышает четыре тысячи строк.
- Windows Bluetooth-подсистема опирается на много эвристик и vendor-specific fallback-ов, которые трудно проверить без hardware/integration fixtures.
- JSON-профили пока не управляют выбором reader/transport/strategy полностью; значительная часть dispatch захардкожена в C++.
- Собственные JSON/protocol parsers увеличивают объём boundary-кода и требуют строгих лимитов и тестов.
- Нет стабильной публичной схемы JSON CLI output и device profile schema validator.
- GUI отсутствует без Qt; Linux не поддерживает self-update и Xiaomi noise control.
- Release packaging частично дублирует install-layout ручным копированием вместо CMake `install()`.
- Именование ChargeView/Battery Monitor неоднородно в продукте, settings, путях и targets.
- Локальная сборка зависит от установленного toolchain; в текущем окружении сохранённый build directory ссылается на отсутствующий Visual Studio 2022 и не может быть пересобран без повторной конфигурации доступным генератором.

## Открытые вопросы

На момент составления плана блокирующих открытых вопросов не осталось. Владелец продукта принял следующие решения:

- low-battery срабатывает при `level <= threshold`, а repeat suppression относится ко всему физическому устройству;
- suppression сбрасывается при переподключении и перезапуске;
- история содержит live-замеры и отдельные offline-события, изменения пишутся сразу, старые точки агрегируются с сохранением 14 дней;
- интервалы более 45 минут не участвуют в ETA, а рост заряда разрывает discharge-сегмент;
- неизвестным TWS-компонентам сохраняется назначение `left/right/case` по порядку;
- при конфликте Windows readers действует явный приоритет источников;
- выбирается один наиболее специфичный device profile;
- некорректные CLI-аргументы дают ошибку и usage;
- production `vX.Y.Z` требует production Authenticode;
- отсутствие Qt при `BATTERY_MONITOR_ENABLE_QT=ON` по-прежнему допускает CLI-only fallback.
