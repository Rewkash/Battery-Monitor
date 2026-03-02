# AGENTS.md

## 1) Цель проекта

`Battery Monitor` - нативное кроссплатформенное приложение на C++, которое:

- работает на Windows и Linux;
- получает данные о Bluetooth-устройствах;
- в базовой версии показывает заряд батареи;
- в следующих версиях расширяется на управление функциями устройства (например ANC/шумодав, режимы, эквалайзер и т.д.).

Ключевое требование платформ:

- Windows: использовать WinRT Bluetooth API;
- Linux: нативно работать в системах с X11 и Wayland (на текущем этапе через CLI, в дальнейшем через GUI-слой).

## 2) Текущий статус (baseline)

Сейчас реализован минимальный рабочий каркас:

- единый интерфейс `IBluetoothBatteryProvider`;
- фабрика платформенных провайдеров;
- Windows-провайдер на WinRT, который читает GATT Battery Service (`0x180F`) и Battery Level (`0x2A19`);
- Linux-провайдер на BlueZ через D-Bus (`org.bluez.Battery1`);
- CLI с обычным и JSON-выводом.

Это фундамент, который нужно расширять без слома архитектуры.

## 3) Структура репозитория

```text
.
|-- CMakeLists.txt
|-- README.md
|-- AGENTS.md
|-- include/
|   `-- core/
|       |-- BatteryTypes.h
|       |-- IBluetoothBatteryProvider.h
|       `-- ProviderFactory.h
|-- src/
|   |-- main.cpp
|   |-- core/
|   |   `-- ProviderFactory.cpp
|   `-- platform/
|       |-- linux/
|       |   |-- BluezBatteryProvider.h
|       |   `-- BluezBatteryProvider.cpp
|       `-- windows/
|           |-- WinRtBatteryProvider.h
|           `-- WinRtBatteryProvider.cpp
`-- .vscode/
    |-- launch.json
    `-- tasks.json
```

## 4) Архитектурные правила

### 4.1 Слои

- `core`:
  - доменные типы;
  - интерфейсы;
  - кроссплатформенная логика, не зависящая от API ОС.
- `platform/windows`:
  - только Windows-специфичный код (WinRT, COM apartment, GATT и т.п.).
- `platform/linux`:
  - только Linux-специфичный код (BlueZ, D-Bus, системные ограничения).
- `main.cpp`:
  - тонкая оболочка: парсинг аргументов, вызов сервиса, вывод.

### 4.2 Что нельзя

- смешивать WinRT/BlueZ код в `core`;
- делать прямые platform-specific include в `main.cpp`;
- завязывать future-фичи (ANC и др.) на текущий CLI-формат.

### 4.3 Как расширять

Каждую новую capability добавлять через интерфейс и отдельные платформенные реализации. Пример:

- `IDeviceControlProvider` для ANC/режимов;
- `IEqualizerProvider` для эквалайзера.

Фабрика должна возвращать платформенный класс, совместимый с интерфейсом, а CLI/GUI должен работать только через интерфейс.

## 5) Платформенные детали

### 5.1 Windows (обязательно WinRT)

- Используем:
  - `Windows.Devices.Enumeration`
  - `Windows.Devices.Bluetooth`
  - `Windows.Devices.Bluetooth.GenericAttributeProfile`
  - `Windows.Storage.Streams`
- Важные моменты:
  - инициализировать apartment (`winrt::init_apartment`);
  - корректно обрабатывать `GattCommunicationStatus`;
  - быть готовым, что часть устройств не отдаёт стандартный Battery Service.

### 5.2 Linux (BlueZ + D-Bus)

- Используем system bus и сервис `org.bluez`.
- Батарея читается через `org.bluez.Battery1` -> `Percentage`.
- Не все устройства публикуют `Battery1`.
- Нельзя предполагать, что наличие `Device1` гарантирует наличие батареи.

### 5.3 X11 / Wayland

Текущий baseline - CLI и не зависит от X11/Wayland напрямую, поэтому запускается в любой сессии.

При добавлении GUI:

- рекомендуемый стек: Qt 6 (нативно поддерживает X11 и Wayland);
- UI должен быть отдельным слоем над `core`;
- Bluetooth код не должен переезжать в UI-модуль.

## 6) Соглашения по коду

- C++20, без platform-кода в `core`;
- минимальные зависимости;
- RAII для системных ресурсов (`DBusMessage`, соединения и т.п.);
- исключения допустимы в boundary-слоях, но не скрывать критические ошибки;
- строки/идентификаторы устройств хранить в UTF-8 на уровне core.

## 7) Сборка и окружение

## 7.1 Windows

Минимум:

- MSVC (Visual Studio 2022 Build Tools);
- Windows SDK 10/11;
- CMake 3.21+;
- генератор: `Visual Studio 17 2022` (рекомендуется).

Команды:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
.\build\Debug\battery-monitor.exe
```

## 7.2 Linux

Минимум:

- `build-essential` или `clang`;
- `cmake`, `pkg-config`, `libdbus-1-dev`, `bluez`.

Команды:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/battery-monitor
```

## 8) Тестовая стратегия

### 8.1 Обязательная ручная проверка

- Windows:
  - Bluetooth наушники/гарнитура подключены;
  - приложение выводит устройство и процент.
- Linux:
  - `bluetoothd` запущен;
  - устройство подключено;
  - `Battery1` действительно присутствует.

### 8.2 Регрессии, которые нужно отслеживать

- приложение падает на устройстве без батарейного сервиса;
- "зависание" при недоступном устройстве;
- некорректная кодировка имени устройства;
- смена API/поведения в новых версиях BlueZ/Windows SDK.

## 9) План расширения (рекомендуемый)

1. Выделить `core` в отдельную библиотеку (оставив CLI как thin-client).
2. Добавить кэш и периодический polling с таймаутами.
3. Добавить события/наблюдение за изменением уровня батареи.
4. Ввести capability-модель для фич устройства:
   - ANC On/Off/Adaptive;
   - Transparency mode;
   - EQ presets/custom bands.
5. Добавить GUI (Qt6):
   - один код UI для Windows/Linux;
   - поддержка X11/Wayland на Linux.

## 10) Правила для будущих агентов

- Перед крупными изменениями сначала стабилизировать контракты интерфейсов в `include/core`.
- Если добавляется новая функция устройства:
  - сначала описать интерфейс в `core`;
  - затем сделать Windows и Linux реализации;
  - затем подключить в CLI/GUI.
- Любой platform-specific код обязан оставаться в `src/platform/<os>`.
- Любые "быстрые хаки" в `main.cpp` считаются временными и должны быть устранены.
- Не ломать существующий JSON-формат без явной миграции.
- Если нет уверенности в поддержке feature устройством, возвращать "capability unavailable", а не падать.

## 11) Известные ограничения baseline

- Нет GUI в текущей версии (только CLI).
- Нет асинхронного обновления в реальном времени.
- Linux-часть зависит от того, публикует ли BlueZ `Battery1`.
- WinRT-часть сейчас ориентирована на стандартный Battery Service и не покрывает vendor-specific протоколы.

## 12) Критерий готовности следующего этапа

Следующий этап можно считать успешным, если:

- выделен стабильный API для device capabilities;
- добавлен минимум один расширенный control use-case (например ANC toggle);
- есть работающий GUI-клиент на Qt6 в Windows + Linux;
- подтверждена работа GUI как минимум в одной X11 и одной Wayland сессии.

