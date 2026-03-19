# Xiaomi Keep Vs Unverified

Дата фиксации: `2026-03-19`

Этот файл отвечает на два вопроса:

- что по Xiaomi уже подтверждено живым запуском и точно оставляем;
- что еще не перепроверено после чистки и пока считаем `unverified`.

## 1. Точно оставляем: подтверждено живым запуском

Эти файлы реально участвовали в получении заряда на текущем наборе Xiaomi-наушников.

### 1.1 Основной orchestration

- `src/platform/windows/WinRtBatteryProvider.cpp`
  - верхняя точка входа `GetDevicesBattery`
  - запускает Xiaomi battery flow

- `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
  - находит connected Bluetooth/TWS candidates
  - дает входные данные для Xiaomi fallback

- `src/platform/windows/shared/WindowsTwsCandidateBatteryCollector.cpp`
  - выбирает Xiaomi TWS-кандидатов
  - решает, когда идти в classic Xiaomi read

### 1.2 Live Xiaomi battery path

- `src/platform/windows/devices/xiaomi/XiaomiBatteryCaches.cpp`
  - live-read wrapper
  - retry
  - fallback на persisted snapshot
  - важно: на текущем прогоне `Redmi AirDots 3 Pro` реально пришли через persisted cache, поэтому прямо сейчас файл нельзя честно назвать мусором

- `src/platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.cpp`
  - открывает `FD2D` RFCOMM socket
  - запускает Xiaomi classic session

- `src/platform/windows/bluetooth/BluetoothSocketUtils.cpp`
  - низкоуровневый send/recv/connect для RFCOMM

- `src/platform/windows/devices/xiaomi/XiaomiClassicBatterySession.cpp`
  - ведет Xiaomi battery session
  - шлет auth start
  - принимает сообщения
  - собирает итоговый snapshot батареи

- `src/platform/windows/devices/xiaomi/XiaomiControlSession.cpp`
  - разбирает входящий поток байтов на Xiaomi messages
  - шлет init requests / report-status ack
  - файл назван неудачно, но реально участвует в battery path

- `src/platform/windows/devices/xiaomi/XiaomiProtocol.cpp`
  - кодирует и декодирует Xiaomi message/frame
  - без него session не сможет говорить с устройством

- `src/platform/windows/devices/xiaomi/XiaomiBatteryCodec.cpp`
  - вытаскивает battery snapshot из Xiaomi payload

- `src/platform/windows/devices/xiaomi/XiaomiBatteryReadings.cpp`
  - нормализует snapshot в `left/right/case/main`
  - считает, достаточно ли данных

### 1.3 Auth внутри battery path

- `src/platform/windows/devices/xiaomi/XiaomiAuth.cpp`
  - считает challenge-response для vendor auth
  - это не Bluetooth pairing, а часть Xiaomi socket protocol

### 1.4 Подтвержденный fallback

- `src/platform/windows/devices/xiaomi/XiaomiPersistentCache.cpp`
  - нужен сейчас как реальный fallback
  - на последнем прогоне именно он спас `Redmi AirDots 3 Pro`, потому что live `FD2D` не поднялся

### 1.5 Подтвержденный mode side-effect

- `src/platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.cpp`
  - из тех же Xiaomi сообщений парсит mode
  - на живом прогоне дал `deviceMode="off"` для `Redmi Buds 4 Pro`

- `src/platform/windows/devices/xiaomi/XiaomiModeCache.cpp`
  - сохраняет распарсенный mode/submode

## 2. Оставляем по фиче, но в этом прогоне не перепроверяли

Эти файлы не участвовали в последнем battery read как hot path, но они нужны для Xiaomi control feature и сейчас их не трогаем.

- `src/platform/windows/devices/xiaomi/XiaomiControlActions.cpp`
  - high-level set mode / set submode

- `src/platform/windows/devices/xiaomi/XiaomiControlConnection.cpp`
  - control session lifecycle

- `src/platform/windows/devices/xiaomi/XiaomiControlSocket.cpp`
  - открывает control socket
  - важно: внутри все еще есть fallback-транспорты `SPP-1101` и `ZMI-1101`
  - после battery cleanup этот control-path отдельно не перепроверялся

- `src/platform/windows/devices/xiaomi/XiaomiHandshake.cpp`
  - отдельный handshake для control path
  - важно: battery path его уже не использует

- `src/ui/BatteryWindow.cpp`
  - UI-слой, который вызывает Xiaomi control API

## 3. Еще не проверили после чистки

Это не значит, что код мусор. Это значит только, что именно на текущем прогоне он не был подтвержден как работающий источник Xiaomi battery.

### 3.1 Общие reader paths

- `src/platform/windows/shared/WindowsBleCandidateBatteryCollector.cpp`
  - общий BLE collector
  - в последнем прогоне Xiaomi результат не пришел через этот hot path

- `src/platform/windows/bluetooth/BleStandardBatteryReader.cpp`
  - стандартный BLE Battery Service
  - нужен как универсальный reader, но не был подтвержден как Xiaomi source в этом прогоне

- `src/platform/windows/bluetooth/BleVendorTripletReader.cpp`
  - vendor BLE triplet fallback
  - ветка есть, но на текущем Xiaomi trace не была решающей

### 3.2 Медленные Xiaomi/TWS fallback ветки

- AEP path внутри `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
- AEP path внутри `src/platform/windows/shared/WindowsTwsCandidateBatteryCollector.cpp`
  - в последнем прогоне был лог `AEP scan skipped because fast candidate scan already found targets`
  - значит эти ветки сейчас `unverified`

### 3.3 Generic scan

- generic scan path в `src/platform/windows/WinRtBatteryProvider.cpp`
- generic scan path в `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
  - в последнем прогоне был лог `Generic device scan skipped`
  - значит для Xiaomi сейчас это `unverified`

## 4. Уже удалено как холодный Xiaomi battery мусор

Этого больше нет в battery path:

- `src/platform/windows/XiaomiAdvertisementSnapshots.cpp`
- `src/platform/windows/XiaomiAdvertisementSnapshots.h`
- `src/platform/windows/HfpBatterySession.cpp`
- `src/platform/windows/HfpBatterySession.h`
- `src/platform/windows/ZmiBatteryCodec.cpp`
- `src/platform/windows/ZmiBatteryCodec.h`
- `src/platform/windows/ZmiSerialBatterySession.cpp`
- `src/platform/windows/ZmiSerialBatterySession.h`

Также уже вырезаны ветки:

- advertisement battery fallback
- `SPP-1101` battery connect
- `ZMI-1101` battery connect
- `HFP-111E` battery fallback
- `RFCOMM-port-15` battery fallback
- SDP dynamic port scan

## 5. Практический вывод

Если смотреть жестко по текущему Xiaomi battery path, то минимально важные файлы сейчас такие:

- `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
- `src/platform/windows/shared/WindowsTwsCandidateBatteryCollector.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiBatteryCaches.cpp`
- `src/platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.cpp`
- `src/platform/windows/bluetooth/BluetoothSocketUtils.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiClassicBatterySession.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiAuth.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiControlSession.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiProtocol.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiBatteryCodec.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiBatteryReadings.cpp`

И дополнительно:

- `src/platform/windows/devices/xiaomi/XiaomiPersistentCache.cpp`
  - нужен только потому, что один из текущих Xiaomi-девайсов пока живет через cache fallback

- `src/platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiModeCache.cpp`
  - это уже side-effect для mode/state, а не голый battery read

