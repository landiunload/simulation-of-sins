# Разработка

Устройство игры описано в [docs/architecture.md](docs/architecture.md).

## Правила

1. Движок подключается только через `laiue::bootstrap` и таблицы сервисов
   модулей, загруженных по имени. Внутренние заголовки движка недоступны.
2. Каждая технология движка необязательна: код проверяет таблицу сервиса и
   продолжает работу без неё. Новый модуль получает случай в
   `simulation_of_sins.client_modules.*`.
3. `src/sim` детерминирован: только стандартный C, никакого времени ОС,
   случайности ОС, потоков без фиксированного порядка и платформенных
   ветвлений.
4. Повторно используемый функционал переносится в
   [laiue](https://github.com/landiunload/laiue) отдельным изменением с
   тестами и контрактом без игровых терминов.

## Проверки

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug --no-tests=error --output-on-failure
cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release --no-tests=error --output-on-failure
git diff --check
```

Ошибок и предупреждений быть не должно. `.clang-format` применяется только к
затронутым файлам; `clang-tidy` из `.clang-tidy` запускается в CI, находки
исправляются, а не подавляются.

## Изменения

Изменение попадает в `main` через pull request с зелёным CI и вливается
squash-коммитом. Сообщение коммита описывает изменение и ничего больше: без
соавторов, подписей и ссылок на сессии. Теги создаются только для релизов.

Разработческий CI собирает `laiue/main`. Теги и ветки `release/*` собираются
с ревизией движка из [engine.lock](engine.lock); её обновляют отдельным
изменением после зелёного CI на новой ревизии.
