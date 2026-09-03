# Разработка

Перед изменением:

1. Не включайте заголовки из соседнего `laiue/src`; доступен только
   установленный SDK.
2. Новая механика сначала остаётся в `src/game`.
3. Перенос в движок выполняется отдельным изменением, когда контракт не
   содержит игровых терминов и имеет самостоятельные тесты.
4. Сервер и сеть не добавляются без отдельного решения по архитектуре игры.

Минимальная проверка изменения:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug --no-tests=error --output-on-failure
git diff --check
```

Для изменений toolchain или публичной границы также проверяются clang-cl и
Release.

Изменение portable игровой логики дополнительно проходит Linux Docker CI.
Linux ARM64 уже проверен отдельным Docker-запуском и назначен нативному
GitHub-hosted ARM64 runner. Для macOS workflow содержит оба native slices:

```sh
cmake --preset linux-gcc
cmake --build --preset linux-gcc-debug --parallel
ctest --preset linux-gcc-debug --no-tests=error

cmake --preset macos-clang-arm64
cmake --build --preset macos-clang-arm64-debug --parallel
ctest --preset macos-clang-arm64-debug --no-tests=error
```

На Linux также проверяется `linux-clang`, на Intel Mac —
`macos-clang-x86_64`. Эти конфигурации headless: зелёный core test нельзя
описывать как готовый Linux/macOS клиент, пока нет соответствующего
window/render/input/audio backend и нативного smoke test.

macOS jobs не запускались локально из текущей Windows-среды; изменение нельзя
считать нативно подтверждённым только потому, что workflow и presets созданы.

Android ARM64 job использует pinned NDK r29 и обязан финально линковать
`libsimulation_of_sins.so`. iOS ARM64 job использует Xcode 26, deployment
target 15.0 и unsigned build-only app. Успешный link не называется APK/IPA,
store-ready клиентом или device test.

Development CI берёт `laiue/main` для раннего обнаружения интеграционных
поломок между репозиториями. Релиз так собирать нельзя, поэтому теги и ветки
`release/*` собираются с immutable engine SHA из [engine.lock](engine.lock):
его выбирает job `engine-revision`, а остальные jobs получают ревизию из его
output. Обновлять lock-файл можно только после зелёной матрицы на новой
ревизии `laiue`, и это отдельный проверяемый commit.

## Mobile

- Mobile shell передаёт явный writable app-container content root; default
  executable directory на mobile намеренно не используется.
- Native mods на Android/iOS выключены. Texture/data packs и shader content
  проходят отдельную platform/store policy review.

## Закрытые консоли

- Публичные изменения сохраняют platform-agnostic game rules и используют
  только публичный API движка.
- Xbox, PlayStation и Nintendo SDK, toolchain, закрытые API и hardware scripts
  принадлежат отдельному private integration repository.
- Mock adapter не подтверждает консольную поддержку; требуются официальный
  build и запуск на dev/test hardware.
- Пока такой закрытой проверки нет, соответствующая консоль не заявлена как
  поддерживаемая платформа.
- Self-hosted console CI не запускается для pull requests и получает только
  одобренные commits защищённой ветки.
