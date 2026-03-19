# Windows Device Reader Map

Дата фиксации: `2026-03-19`

## Классы устройств и подтвержденные пути чтения заряда

### 1. Мышь / обычное BLE-устройство

Пример: `DELUX`

Подтвержденный путь:

- `src/platform/windows/WinRtBatteryProvider.cpp`
- `src/platform/windows/shared/WindowsBleCandidateBatteryCollector.cpp`
- `src/platform/windows/bluetooth/BleStandardBatteryReader.cpp`

Смысл:

- провайдер запускает общий сбор;
- BLE collector открывает `BluetoothLEDevice`;
- standard BLE reader читает стандартный Battery Service.

### 2. Клавиатура / обычное BLE-устройство

Пример: `K86BT 5.0_1`

Подтвержденный путь:

- `src/platform/windows/WinRtBatteryProvider.cpp`
- `src/platform/windows/shared/WindowsBleCandidateBatteryCollector.cpp`
- `src/platform/windows/bluetooth/BleStandardBatteryReader.cpp`

Смысл:

- путь тот же, что у мышки;
- отдельного keyboard-specific reader нет.

### 3. Телефон

Пример: `POCO F3`

Подтвержденный путь:

- `src/platform/windows/WinRtBatteryProvider.cpp`
- `src/platform/windows/shared/WindowsBatteryProviderSupport.cpp`
- `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
- `src/platform/windows/devices/phone/BluetoothPnpHints.cpp`
- `src/platform/windows/bluetooth/BluetoothPnpUtils.cpp`

Смысл:

- телефон распознается эвристикой;
- затем берется special `Phone PnP battery hint`;
- через общий BLE/generic path телефон на текущей машине не читается.

### 4. Контроллер

Подтвержденный кодовый путь:

- `src/platform/windows/WinRtBatteryProvider.cpp`
- `src/platform/windows/shared/WindowsBatteryProviderSupport.cpp`
- `src/platform/windows/shared/WindowsBatteryQueryReaders.cpp`
- `src/platform/windows/devices/controller/WindowsControllerBatteryReader.cpp`

Внутри special reader:

- сначала `GameInput`;
- затем `DualShock HID fallback`;
- плюс локальный cache route.

Смысл:

- у контроллера есть отдельный special reader;
- это не общий BLE battery path.

### 5. Xiaomi / Redmi TWS

Подтвержденный путь:

- `src/platform/windows/WinRtBatteryProvider.cpp`
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
- `src/platform/windows/devices/xiaomi/XiaomiPersistentCache.cpp`

Дополнительно для mode/control:

- `src/platform/windows/devices/xiaomi/XiaomiControlActions.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiControlConnection.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiControlSocket.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiHandshake.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiModeCache.cpp`
- `src/platform/windows/devices/xiaomi/XiaomiNoiseModeCodec.cpp`

## Целевая раскладка по папкам

### Оставить в корне `src/platform/windows`

- `WinRtBatteryProvider.cpp`
- `WinRtBatteryProvider.h`
- `PlatformCommandDispatcher.cpp`

Причина:

- это platform entrypoints;
- они связывают все подмодули, а не относятся к одному классу устройств.

### Перенести в `src/platform/windows/shared`

- `BatteryComponentNaming.cpp`
- `BatteryComponentNaming.h`
- `WindowsBatteryAggregation.cpp`
- `WindowsBatteryAggregation.h`
- `WindowsBatteryEntryUtils.cpp`
- `WindowsBatteryEntryUtils.h`
- `WindowsBatteryProviderSupport.cpp`
- `WindowsBatteryProviderSupport.h`
- `WindowsBatteryQueryReaders.cpp`
- `WindowsBatteryQueryReaders.h`
- `WindowsBleCandidateBatteryCollector.cpp`
- `WindowsBleCandidateBatteryCollector.h`
- `WindowsTwsCandidateBatteryCollector.cpp`
- `WindowsTwsCandidateBatteryCollector.h`
- `WindowsBluetoothAddressUtils.cpp`
- `WindowsBluetoothAddressUtils.h`
- `WindowsBluetoothConstants.h`
- `WindowsBluetoothTargetResolver.h`
- `WindowsDeviceInfoProperties.cpp`
- `WindowsDeviceInfoProperties.h`
- `BluetoothVisualHintProperties.h`

Причина:

- это общий Windows orchestration/query/property/vocabulary слой;
- он используется несколькими классами устройств сразу.

### Перенести в `src/platform/windows/bluetooth`

- `BleCandidateEnumeration.cpp`
- `BleCandidateEnumeration.h`
- `BleStandardBatteryReader.cpp`
- `BleStandardBatteryReader.h`
- `BleVendorTripletReader.cpp`
- `BleVendorTripletReader.h`
- `BluetoothPnpUtils.cpp`
- `BluetoothPnpUtils.h`
- `BluetoothSocketUtils.cpp`
- `BluetoothSocketUtils.h`

Причина:

- это общий низкоуровневый Bluetooth transport / BLE / PnP слой;
- он не привязан к одному конкретному типу устройства.

### Перенести в `src/platform/windows/devices/phone`

- `BluetoothPnpHints.cpp`
- `BluetoothPnpHints.h`

Причина:

- сейчас special battery path телефона живет именно здесь.

### Перенести в `src/platform/windows/devices/controller`

- `WindowsControllerBatteryReader.cpp`
- `WindowsControllerBatteryReader.h`

Причина:

- это отдельный special controller reader.

### Перенести в `src/platform/windows/devices/xiaomi`

- `ClassicBluetoothBatteryFallback.cpp`
- `ClassicBluetoothBatteryFallback.h`
- `XiaomiAuth.cpp`
- `XiaomiAuth.h`
- `XiaomiBatteryCaches.cpp`
- `XiaomiBatteryCaches.h`
- `XiaomiBatteryCodec.cpp`
- `XiaomiBatteryCodec.h`
- `XiaomiBatteryReadings.cpp`
- `XiaomiBatteryReadings.h`
- `XiaomiClassicBatterySession.cpp`
- `XiaomiClassicBatterySession.h`
- `XiaomiControlActions.cpp`
- `XiaomiControlActions.h`
- `XiaomiControlConnection.cpp`
- `XiaomiControlConnection.h`
- `XiaomiControlSession.cpp`
- `XiaomiControlSession.h`
- `XiaomiControlSocket.cpp`
- `XiaomiControlSocket.h`
- `XiaomiHandshake.cpp`
- `XiaomiHandshake.h`
- `XiaomiModeCache.cpp`
- `XiaomiModeCache.h`
- `XiaomiNoiseModeCodec.cpp`
- `XiaomiNoiseModeCodec.h`
- `XiaomiPersistentCache.cpp`
- `XiaomiPersistentCache.h`
- `XiaomiProtocol.cpp`
- `XiaomiProtocol.h`

Причина:

- это vendor-specific Xiaomi stack;
- он не должен лежать вперемешку с общими Windows readers.

