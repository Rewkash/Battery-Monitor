# План улучшений ChargeView

План составлен после полного read-only аудита tracked-кода, конфигурации, профилей, CI/release tooling и документации. Реализация должна идти строго по этапам. Архитектурные переписывания не предполагаются: предпочтительны локальные исправления с regression-тестами.

## Этап 1 — Критично

- [x] **Исключить завершение посторонних процессов при запуске GUI.**
  - **Файлы:** `src/main.cpp`, при необходимости небольшой IPC/single-instance helper.
  - **Изменение:** отказаться от поиска и `TerminateProcess` всех процессов с basename `battery-monitor.exe`; активировать или корректно завершать только экземпляр той же установки и пользовательской сессии, проверяя identity/full path.
  - **Ожидаемый эффект:** устранение локальной DoS, потери данных и PID reuse race.
  - **Сложность:** высокая.

- [x] **Устранить потенциальный use-after-free в BLE notification callback.**
  - **Файлы:** `src/platform/windows/bluetooth/BleVendorTripletReader.cpp`.
  - **Изменение:** не захватывать локальное состояние callback по ссылке; использовать lifetime-safe shared state, детерминированно снимать handler и дожидаться завершения callback перед уничтожением состояния.
  - **Ожидаемый эффект:** предотвращение редких падений и повреждения памяти при timeout/cancellation.
  - **Сложность:** средняя.

- [x] **Ограничить входные размеры Xiaomi protocol decoders.**
  - **Файлы:** codecs/parsers в `src/platform/windows/devices/xiaomi`, включая classic/RFCOMM frame decoding.
  - **Изменение:** ввести жёсткие пределы frame/payload/buffer, отвергать невозможные length fields и сбрасывать повреждённый stream без неограниченного накопления.
  - **Ожидаемый эффект:** защита от исчерпания памяти и зависания на повреждённых либо враждебных Bluetooth-данных.
  - **Сложность:** средняя.

- [x] **Сделать persistent Xiaomi cache потокобезопасным и атомарным.**
  - **Файлы:** persistent-cache implementation в `src/platform/windows/devices/xiaomi`, места чтения/записи из sessions/provider.
  - **Изменение:** синхронизировать process-wide state, корректно учитывать каждый cache path, записывать через temporary file + flush + atomic replace и безопасно переживать повреждённый файл.
  - **Ожидаемый эффект:** отсутствие data race, смешивания разных путей и потери всего cache при сбое записи.
  - **Сложность:** средняя.

- [ ] **Запретить неподписанные и test-signed production-релизы.**
  - **Файлы:** `.github/workflows/release.yml`, `docs/release-process.md`, `scripts/release/verify_release.py`.
  - **Изменение:** для `vX.Y.Z` обязательно требовать production PFX/password, запрещать fallback на test certificate и проверять Authenticode всех публикуемых PE/MSI; test certificate разрешать только для `vX.Y.Z-test.N`.
  - **Ожидаемый эффект:** production release нельзя случайно опубликовать без доверенной подписи издателя.
  - **Сложность:** низкая.

## Этап 2 — Важно

- [x] **Добавить deadline и cooperative cancellation для provider operations и завершения GUI.**
  - **Файлы:** `src/ui/BatteryWindow.{h,cpp}`, `src/platform/windows/WinRtBatteryProvider.*`, Xiaomi sessions/transports, общие timeout helpers.
  - **Изменение:** исключить неограниченный `join()` на UI thread; передавать cancellation/deadline в долгие операции; после WinRT `Cancel()` корректно завершать lifecycle async operation.
  - **Ожидаемый эффект:** GUI и updater не зависают при закрытии или недоступном Bluetooth-устройстве.
  - **Сложность:** высокая.

- [x] **Ограничить сетевые ответы updater до `readAll()`.**
  - **Файлы:** `src/update/UpdateService.cpp`, `src/update/UpdateSecurity.h`.
  - **Изменение:** проверять `Content-Length`, читать manifest/signature/package инкрементально, abort при превышении лимита; задать малый отдельный лимит подписи.
  - **Ожидаемый эффект:** защита GUI от memory/disk exhaustion и сетевого DoS.
  - **Сложность:** средняя.

- [x] **Исправить генерацию JSON CLI.**
  - **Файлы:** `src/app/BatteryOutputFormatter.cpp`.
  - **Изменение:** экранировать все control characters `U+0000..U+001F` как валидный JSON либо использовать проверенную JSON-библиотеку без изменения текущей схемы.
  - **Ожидаемый эффект:** `--json` всегда выдаёт синтаксически корректный документ для любых имён устройств.
  - **Сложность:** низкая.

- [x] **Исправить Unicode parsing профилей.**
  - **Файлы:** `src/core/DeviceProfiles.cpp`.
  - **Изменение:** корректно декодировать `\uXXXX`, non-ASCII и surrogate pairs в UTF-8; не заменять символы на `?`; добавить ограничения depth/size, если они отсутствуют.
  - **Ожидаемый эффект:** профили с Unicode корректно загружаются и совпадают с именами устройств.
  - **Сложность:** средняя.

- [x] **Сделать ошибки CLI явными.**
  - **Файлы:** `src/app/CommandLineOptions.{h,cpp}`, `src/main.cpp`, platform command dispatchers.
  - **Изменение:** неизвестные флаги, отсутствующие значения, несовместимые `--cli/--gui` и некорректные noise/submode arguments должны давать понятный usage и ненулевой exit code.
  - **Ожидаемый эффект:** скрипты не получают ложный успешный результат и команды не выполняются частично.
  - **Сложность:** средняя.

- [x] **Зафиксировать детерминированный приоритет Windows live readings.**
  - **Файлы:** `src/platform/windows/WinRtBatteryProvider.cpp`, `src/platform/windows/shared/WindowsBatteryAggregation.{h,cpp}`, provider diagnostics/documentation.
  - **Изменение:** назначить источникам документированный приоритет по достоверности/свежести; одинаковый компонент не должен зависеть от порядка завершения readers; конфликт отражать в diagnostics.
  - **Ожидаемый эффект:** стабильные показания без случайного last-writer-wins.
  - **Сложность:** средняя.

- [x] **Выбирать один наиболее специфичный device profile.**
  - **Файлы:** `include/core/DeviceProfiles.h`, `src/core/DeviceProfiles.cpp`, Windows profile consumers, `profiles/devices/README.md`.
  - **Изменение:** определить specificity, детерминированный tie-breaker и warning для равнозначного конфликта; не объединять несовместимые family/capabilities нескольких профилей.
  - **Ожидаемый эффект:** предсказуемая классификация и отсутствие зависимости от порядка файлов.
  - **Сложность:** средняя.

- [x] **Исправить low-battery threshold и suppression.**
  - **Файлы:** `src/ui/BatteryWindow.{h,cpp}`.
  - **Изменение:** уведомлять при `level <= threshold`; вести repeat interval на физическое устройство, а не отдельно на `left/right/case`; по принятому решению сбрасывать suppression при disconnect/reconnect и не сохранять между запусками.
  - **Ожидаемый эффект:** корректное срабатывание на пороге без одновременного спама от компонентов.
  - **Сложность:** средняя.

- [x] **Расширить модель истории live + offline events и исправить retention.**
  - **Файлы:** `src/ui/BatteryHistoryStore.{h,cpp}`, `src/ui/BatteryHistoryDialog.*`, `src/ui/BatteryStatistics.*`.
  - **Изменение:** записывать изменения заряда/режима сразу, отдельно фиксировать offline events без выдачи cached value за live; хранить 14 дней, агрегируя старые точки вместо простого обрезания до 2048.
  - **Ожидаемый эффект:** история объясняет разрывы подключения, сохраняет значимые события и не теряет временной охват.
  - **Сложность:** высокая.

- [x] **Уточнить фильтрацию данных для ETA.**
  - **Файлы:** `src/ui/BatteryRuntimeEstimator.{h,cpp}`, `src/ui/BatteryStatistics.*`.
  - **Изменение:** исключать интервалы более 45 минут; рост уровня трактовать как границу charging/disconnected-сегмента и не использовать для discharge rate; начинать новый discharge segment после роста.
  - **Ожидаемый эффект:** повреждённые, несопоставимые и прошедшие через зарядку точки не искажают прогноз.
  - **Сложность:** средняя.

- [ ] **Сделать release notes безопасными при открытии ссылок.**
  - **Файлы:** `src/ui/UpdateDialog.cpp`, при необходимости `src/update/UpdateManifest.cpp`.
  - **Изменение:** перехватывать ссылки, разрешать только `https`, показывать домен/подтверждение и не открывать произвольные URI schemes автоматически.
  - **Ожидаемый эффект:** подписанный, но ошибочный или скомпрометированный release metadata не запускает опасные local URI handlers.
  - **Сложность:** низкая.

- [ ] **Сократить утечки чувствительных Bluetooth-данных в логах.**
  - **Файлы:** logging/diagnostics в `src/platform/windows/shared`, Bluetooth/Xiaomi readers, UI diagnostics.
  - **Изменение:** по умолчанию редактировать device IDs/addresses и protocol payloads; raw dumps включать только явным debug opt-in, ограничивать размер и rotation.
  - **Ожидаемый эффект:** меньше персональных/device identifiers и vendor traffic в файлах, bug reports и support logs.
  - **Сложность:** средняя.

- [ ] **Добавить базовый автоматизированный test harness до дальнейших рискованных исправлений.**
  - **Файлы:** `CMakeLists.txt`, новый `tests/`, `.github/workflows/build.yml`, `.github/workflows/release.yml`.
  - **Изменение:** подключить CTest; покрыть JSON formatter, CLI parser, profile parser/matching, aggregation, Xiaomi codecs/cache, history/ETA и update manifest/security; запускать tests до packaging и доступа к release secrets.
  - **Ожидаемый эффект:** критические регрессии блокируют merge/release.
  - **Сложность:** высокая.

## Этап 3 — Желательно

- [ ] **Добавить Linux CI и smoke/integration fixtures BlueZ D-Bus.**
  - **Файлы:** `.github/workflows/build.yml`, `src/platform/linux/BluezBatteryProvider.cpp`, новый `tests/fixtures`.
  - **Изменение:** собирать CLI на Ubuntu с `libdbus-1-dev`; parser/mapper проверять на записанных `GetManagedObjects` fixtures без реального hardware.
  - **Ожидаемый эффект:** Linux compilation и mapping перестают быть непроверяемыми.
  - **Сложность:** средняя.

- [ ] **Усилить error handling и timeout Linux D-Bus path.**
  - **Файлы:** `src/platform/linux/BluezBatteryProvider.{h,cpp}`.
  - **Изменение:** гарантировать конечный timeout, различать BlueZ unavailable/permission/malformed reply, освобождать D-Bus resources на каждом early return и выдавать полезную диагностику.
  - **Ожидаемый эффект:** CLI не зависает и сообщает реальную причину отсутствия данных.
  - **Сложность:** средняя.

- [x] **Валидировать device profile schema и дубликаты.**
  - **Файлы:** `src/core/DeviceProfiles.cpp`, `profiles/devices/README.md`, JSON-профили.
  - **Изменение:** проверять обязательные поля, типы, schemaVersion, допустимые platform/category/capability values, duplicate IDs и невозможные пустые matchers; ошибки содержат source path.
  - **Ожидаемый эффект:** некорректный профиль не меняет поведение молча.
  - **Сложность:** средняя.

- [x] **Сделать обработку повреждённых локальных JSON/INI state безопасной и наблюдаемой.**
  - **Файлы:** `src/ui/BatteryHistoryStore.cpp`, `src/update/UpdateState.cpp`, `src/ui/BatteryWindowSettings.cpp`, Windows cache implementations.
  - **Изменение:** атомарная запись, backup/recovery, size limits и диагностируемый fallback без полного silent reset.
  - **Ожидаемый эффект:** crash/power loss не уничтожает историю, настройки и anti-rollback state.
  - **Сложность:** средняя.

- [x] **Устранить неоднозначное назначение TWS-компонентов, сохранив принятую эвристику.**
  - **Файлы:** `src/platform/windows/shared/BatteryComponentNaming.cpp`, aggregation tests.
  - **Изменение:** продолжать назначение `left/right/case` по порядку, но сделать порядок стабильным внутри snapshot и явно маркировать эвристическое происхождение в diagnostics.
  - **Ожидаемый эффект:** UI остаётся удобным, а перестановки и ложная уверенность становятся заметны.
  - **Сложность:** низкая.

- [x] **Проверять update/release pipeline до публикации.**
  - **Файлы:** `.github/workflows/release.yml`, `scripts/release/verify_release.py`, release scripts.
  - **Изменение:** запускать CTest и CLI smoke; проверять bundle traversal/duplicates/size/hash, manifest signature, MSI identity, version consistency и dry-run layout до `gh release`.
  - **Ожидаемый эффект:** скомпилированный, но неработоспособный или несогласованный пакет не публикуется.
  - **Сложность:** средняя.

- [x] **Добавить CMake install rules и использовать единый staging layout.**
  - **Файлы:** `CMakeLists.txt`, `.github/workflows/release.yml`, `scripts/release/build_bundle.py`, `scripts/release/build_msi.py`.
  - **Изменение:** описать runtime binaries, Qt deployment, profiles и resources через `install()`; packaging строить из `cmake --install`.
  - **Ожидаемый эффект:** меньше ручного копирования и расхождений ZIP/MSI/bundle.
  - **Сложность:** средняя.

- [x] **Добавить warning policy для GCC/Clang и статический анализ.**
  - **Файлы:** `CMakeLists.txt`, `.github/workflows/build.yml`.
  - **Изменение:** включить разумные `-Wall -Wextra -Wpedantic`, MSVC `/analyze` или clang-tidy отдельным неблокирующим этапом, затем постепенно сделать blocking.
  - **Ожидаемый эффект:** portability, lifetime и conversion defects обнаруживаются раньше.
  - **Сложность:** низкая.

- [ ] **Зафиксировать зависимости и автоматизировать проверку их обновлений.**
  - **Файлы:** `CMakeLists.txt`, `scripts/release/requirements.txt`, GitHub Actions.
  - **Изменение:** сохранять immutable pins/checksums для FetchContent и Actions, использовать hash-locked Python dependencies и Dependabot/Renovate; обновлять только после CI.
  - **Ожидаемый эффект:** снижение supply-chain риска и воспроизводимая сборка.
  - **Сложность:** средняя.

## Этап 4 — Технический долг / рефакторинг

- [x] **Декомпозировать `BatteryWindow` без изменения поведения.** *(начальный шаг: notification-логика вынесена в `LowBatteryNotifier`; продолжать по мере добавления тестов)*
  - **Файлы:** `src/ui/BatteryWindow.{h,cpp}` и новые локальные UI helpers/controllers.
  - **Изменение:** после появления тестов отделить refresh orchestration, notifications, tray, history и updater presentation небольшими шагами.
  - **Ожидаемый эффект:** меньшая связанность и безопаснее последующие UI-изменения.
  - **Сложность:** высокая.

- [x] **Сократить дублирование Windows async/timeout и reader fallback logic.** *(общий хелпер `WindowsAsyncWait.h`; переведён `WindowsBatteryAggregation`, остальные call sites мигрируют при следующих правках)*
  - **Файлы:** WinRT/BLE/AEP/PnP/Xiaomi readers в `src/platform/windows`.
  - **Изменение:** выделять общие lifetime-safe helpers только для реально повторяющихся шаблонов после regression-тестов.
  - **Ожидаемый эффект:** единые timeout/cancellation semantics и меньше расхождений error handling.
  - **Сложность:** высокая.

- [ ] **Продвинуть profiles к реальному reader/transport dispatch.**
  - **Файлы:** `include/core/DeviceProfiles.h`, `src/core/DeviceProfiles.cpp`, Windows reader selection, `profiles/devices`.
  - **Изменение:** после стабилизации schema постепенно использовать `reader`, `transport`, `strategy`, уменьшая hardcoded model-name checks; неизвестные значения fail closed.
  - **Ожидаемый эффект:** новые модели добавляются данными, а не изменениями по нескольким C++-файлам.
  - **Сложность:** высокая.

- [x] **Унифицировать product naming без миграционной поломки.**
  - **Файлы:** CMake targets/generated identity, QSettings identifiers, installer paths, docs/UI strings.
  - **Изменение:** определить ChargeView как display name и BatteryMonitor как стабильный internal identity; документировать и тестировать миграцию settings/install state.
  - **Ожидаемый эффект:** меньше путаницы без потери пользовательских настроек и upgrade compatibility.
  - **Сложность:** средняя.

- [ ] **Описать стабильные форматы данных.**
  - **Файлы:** `README.md`, `profiles/devices/README.md`, docs для CLI JSON/history/update state.
  - **Изменение:** документировать schema/versioning, поля, encoding, compatibility и limits.
  - **Ожидаемый эффект:** безопаснее автоматизация и миграции форматов.
  - **Сложность:** низкая.

## Идеи по улучшению

- Добавить corpus/fuzz tests для собственных JSON, BLE/RFCOMM и `.bmup` parsers.
- Создать обезличенные recorded fixtures реальных Bluetooth-ответов для deterministic reader tests без hardware.
- Добавить sanitizers на Clang/Linux и, где возможно, Windows ASan для protocol/cache tests.
- Добавить SBOM и provenance/attestation release-артефактов.
- Публиковать machine-readable device-profile schema и небольшой validator CLI для авторов профилей.
- Добавить privacy-настройку экспорта diagnostics с предварительным просмотром редактируемых данных.
- Ввести отдельный nightly hardware-in-the-loop набор для поддерживаемых Xiaomi/Redmi моделей, не блокируя обычный CI.

## Проверка выполнения

Минимальная проверка после каждого этапа:

1. `cmake --fresh -S . -B build -G "Visual Studio 17 2022" -A x64`
2. `cmake --build build --config Debug`
3. `ctest --test-dir build -C Debug --output-on-failure`
4. `build\Debug\battery-monitor-cli.exe --version`
5. CLI text/JSON smoke с mock/fixture provider.
6. Linux CLI-only configure/build/CTest в CI.
7. Для release-изменений — test-tag dry run, manifest/bundle/MSI verification и Authenticode verification до публикации.

В текущем локальном окружении существующий `build` нельзя использовать для проверки: он настроен на отсутствующий Visual Studio 2022. Перед реализацией потребуется доступный MSVC toolchain либо чистая конфигурация другим поддерживаемым генератором.
