# Xiaomi Runtime Map

Дата живой проверки: `2026-03-19`

## Что реально дало батарею на текущих Xiaomi

Текущий живой запуск `BATTERY_MONITOR_DEBUG=1` + `battery-monitor-cli --json` подтвердил:

- `Redmi Buds 4 Pro`
  - живой battery path:
    - `src/platform/windows/WinRtBatteryProvider.cpp`
    - `src/platform/windows/shared/WindowsTwsCandidateBatteryCollector.cpp`
    - `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiBatteryCaches.cpp`
    - `src/platform/windows/devices/xiaomi/ClassicBluetoothBatteryFallback.cpp`
    - `src/platform/windows/bluetooth/BluetoothSocketUtils.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiClassicBatterySession.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiHandshake.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiAuth.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiControlSession.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiProtocol.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiBatteryCodec.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiBatteryReadings.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiModeCache.cpp`
  - факт из trace:
    - `Classic RFCOMM: connected via FD2D`
    - mode parsed as `off`
    - батарея собрана как `left=85`, `case=36`

- `Redmi AirDots 3 Pro`
  - живой battery path:
    - `src/platform/windows/WinRtBatteryProvider.cpp`
    - `src/platform/windows/shared/WindowsTwsCandidateBatteryCollector.cpp`
    - `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiBatteryCaches.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiPersistentCache.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiBatteryCodec.cpp`
    - `src/platform/windows/devices/xiaomi/XiaomiBatteryReadings.cpp`
  - факт из trace:
    - `FD2D` не подключился
    - итог взят из `persisted cache`
    - батарея отдана как `right=100`, `case=95`

## Что оставлено для Xiaomi control

Текущий control stack:

- `src/platform/windows/WinRtBatteryProvider.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiControlActions.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiControlConnection.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiControlSocket.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiHandshake.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiControlSession.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiProtocol.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiModeCache.cpp`

UI вызывает этот стек через:

- `src/ui/BatteryWindow.cpp`

## Что оставлено как универсальный battery layer

Это не Xiaomi-specific, но оставлено как общий полезный reader layer:

- `src/platform/windows/shared/WindowsBleCandidateBatteryCollector.cpp`
- `src/platform/windows/bluetooth/BleStandardBatteryReader.cpp`
- `src/platform/windows/bluetooth/BleVendorTripletReader.cpp`
- `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
- `src/platform/windows/devices/controller/WindowsControllerBatteryReader.cpp`
- `src/platform/windows/devices/phone/BluetoothPnpHints.cpp`

## Что удалено как холодный Xiaomi/ZMI/HFP мусор

Удаленные файлы:

- `src/platform/windows/XiaomiAdvertisementSnapshots.cpp`
- `src/platform/windows/XiaomiAdvertisementSnapshots.h`
- `src/platform/windows/HfpBatterySession.cpp`
- `src/platform/windows/HfpBatterySession.h`
- `src/platform/windows/ZmiBatteryCodec.cpp`
- `src/platform/windows/ZmiBatteryCodec.h`
- `src/platform/windows/ZmiSerialBatterySession.cpp`
- `src/platform/windows/ZmiSerialBatterySession.h`

Удаленные холодные ветки из Xiaomi battery path:

- advertisement fallback scan/cache
- `SPP-1101` battery connect path
- `ZMI-1101` battery connect path
- `HFP-111E` battery fallback
- `RFCOMM-port-15` battery fallback
- SDP dynamic port scan
- ZMI pre-auth battery probe
- ZMI serial battery fallback

## Текущее состояние после чистки

После удаления холодных веток живой запуск все еще отдает:

- `Redmi AirDots 3 Pro`: `right=100`, `case=95`, `isCached=true`
- `Redmi Buds 4 Pro`: `left=85`, `case=36`, `deviceMode="off"`

Итог: для текущего набора Xiaomi-наушников battery path сужен до `FD2D classic session + persisted cache`, а control path оставлен отдельно.

