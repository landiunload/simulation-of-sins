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
