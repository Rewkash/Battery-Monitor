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

## Выбор наиболее специфичного профиля

Если запросу устройства соответствует несколько профилей, выбирается ровно
один профиль — объединения (merging) профилей не происходит. Специфичность
определяется детерминированно:

1. Суммарная длина совпавших токенов `match.deviceIdContains` (совпадение по
   идентификатору устройства специфичнее совпадения по имени).
2. При равенстве — суммарная длина совпавших токенов `match.nameContains`.
3. При полном равенстве — лексикографически наименьший `id` профиля.

О конфликтах одинаковой специфичности сообщается в заметках результата выбора
(`DeviceProfileSelection::notes`).

## Валидация

Профили с ошибками отклоняются целиком; ошибка содержит путь к файлу и
причину. Проверяются:

- `schemaVersion` обязателен и должен быть числом `1`;
- `id` обязателен и не может быть пустым или из одних пробелов; дубликаты
  `id` по всем файлам отклоняются (в сообщении указываются оба файла);
- типы полей (строки, массивы строк, булевы значения, объекты);
- `platforms`: допустимы `any`, `windows`, `linux`, `macos`, `android`, `ios`;
- `deviceCategories`: допустимы `tws`, `headphone`, `headset`, `earbuds`,
  `speaker`, `phone`, `tablet`, `laptop`, `watch`, `mouse`, `keyboard`,
  `controller`, `pen`, `other`;
- `battery.transport` / `noiseControl.transport`: допустимы `rfcomm`, `ble`,
  `hid`, `usb`, `serial`, `any`;
- для включённой capability (`enabled: true`) поле `reader` должно быть
  непустым;
- `match` обязателен и должен содержать хотя бы один непустой токен в
  `nameContains` или `deviceIdContains`; пустые/пробельные токены отклоняются.

Строки парсятся как корректный UTF-8: поддерживаются escape-последовательности
`\uXXXX`, включая суррогатные пары; некорректные последовательности
отклоняются с ошибкой. Глубина вложенности JSON ограничена 64 уровнями,
максимальный размер файла — 16 МиБ.
