# Device Profiles

`ChargeView` ищет JSON-профили устройств в папке `profiles/devices`.

Порядок поиска:
- `BATTERY_MONITOR_PROFILE_DIR`, если задана переменная окружения
- `profiles/devices` в текущей директории запуска
- `profiles/devices` в родительских директориях текущей папки

Назначение профилей на текущем этапе:
- расширять распознавание семейств и категорий устройств без правки C++-эвристик;
- описывать, какой reader/transport/strategy ожидается для батареи и управления;
- готовить фундамент для будущего profile-driven battery/control runtime.

Текущий runtime уже использует профили для:
- `deviceCategories`
- `family`
- `match.nameContains`
- `match.deviceIdContains`

Минимальный пример:

```json
{
  "schemaVersion": 1,
  "id": "vendor.model",
  "displayName": "Vendor Model",
  "platforms": ["windows"],
  "vendor": "vendor",
  "family": "vendor_family",
  "deviceCategories": ["tws"],
  "match": {
    "nameContains": ["Vendor Model"],
    "deviceIdContains": ["bluetooth"]
  },
  "battery": {
    "enabled": true,
    "reader": "vendor_reader",
    "transport": "rfcomm"
  },
  "noiseControl": {
    "enabled": true,
    "strategy": "vendor_strategy"
  }
}
```
