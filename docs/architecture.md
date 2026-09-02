# Архитектура Simulation of sins

## Граница проекта

`Simulation of sins` — приложение. `laiue` — независимый встраиваемый
движок. Между репозиториями существует только установленный C API и CMake
package `laiue::*`.

```text
main_windows
    -> app/application
        -> game/foundation_world + game/rebase_policy + game/frame_timing
        -> public laiue SDK
            -> window/input/content/world/mesher/render/scene
```

`main_windows` является composition root. `Application` единолично владеет
окном, вводом, каталогом содержимого, renderer, миром и streaming. Чистые
игровые функции не знают о Win32 или D3D12 и проверяются без окна.

На Linux и macOS CI собирает только headless composition: portable game
library, тесты и доступные core-модули установленного `laiue` SDK. Эти jobs
намеренно не подменяют отсутствующие platform clients. Будущие Linux/macOS
composition roots подключат window/render/input/audio через публичные
capabilities движка, не через ветвление внутри `src/game`.

Linux ARM64 composition фактически прошёл Docker-сборку и CTest; нативный
GitHub-hosted ARM64 job закрепляет эту границу в CI. macOS arm64/x86_64 jobs
настроены на нативные runners, но не исполнялись в текущей Windows-среде.

Android и iOS используют source-superbuild только на platform integration
границе: игровые правила по-прежнему зависят от публичных `laiue::*` targets.
Android CI линкует итоговую `.so`, Apple CI — минимальный unsigned app bundle.
Их Debug/no-LTO closure-цели whole-archive включают каждый object game core и
движковых `world`, `physics`, `content`, `mod` без section GC; отдельные
Release-цели проверяют финальную оптимизированную LTO/dead-strip линковку.
Оба профиля держат native mods выключенными.
Platform application shell владеет lifecycle, resource/data roots,
touch/controller input, audio, suspend/resume и store packaging.

## Правила владения

- Каждый engine handle уничтожается тем публичным API, которым создан.
- `ChunkStreaming` уничтожается до `World` и `Renderer`.
- Rebase выполняется только после `ChunkStreamingPause`; камера сдвигается в
  той же остановленной границе, затем streaming возобновляется.
- Абсолютные координаты остаются в `World`. GPU получает только положение
  относительно целочисленного render origin около камеры.
- Ошибка создания любого обязательного ресурса приводит к симметричному
  освобождению уже созданных ресурсов.

## Когда код переносится в laiue

Кандидат должен одновременно:

1. не зависеть от терминов, данных и правил конкретной игры;
2. иметь узкий C17 API и однозначного владельца ресурсов;
3. быть полезным другому приложению либо закрывать общий platform/runtime
   контракт;
4. иметь тесты в репозитории движка и документированную платформенную
   границу.

До выполнения этих условий код остаётся в игре. Так проект не превращается
во вторую копию движка, а движок — в скрытый монолит одной игры.

## Внешние платформенные adapters

Лицензируемая консольная платформа подключается за пределами публичного
репозитория. Публичная часть может описывать только необходимые capabilities
и portable composition contract. Конкретные SDK headers, toolchain, renderer,
input/audio/storage bindings, packaging и hardware tests находятся в
закрытом integration repository.

Xbox, PlayStation и Nintendo пока не заявлены как поддерживаемые платформы.
Такой статус можно изменить только после нативной сборки и запуска одобренного
commit на официальном dev/test hardware. До этого существует лишь публичный
контракт для будущей adapter boundary. Возможность native mods, shader packs
и пользовательского содержимого также не предполагается по PC-поведению.

Steam Deck относится к Linux x86_64, Android TV/Quest — к Android ARM64,
iPadOS — к iOS. Общий core не означает готовый форм-фактор: renderer, input,
audio, memory/thermal budgets и UX проверяются отдельно. Web target потребует
нового sandbox/filesystem/thread adapter и сейчас находится вне scope.

## Текущий scope

Стартовая authored scene — технический fixture, а не процедурный ландшафт и
не утверждение о будущем дизайне. Физический player controller, сохранения,
моды игры и игровые системы будут добавляться отдельными vertical slices.
