#ifndef SIMULATION_OF_SINS_APP_MEMORY_PROFILE_H
#define SIMULATION_OF_SINS_APP_MEMORY_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

// Карта памяти всего процесса в устоявшемся состоянии.
//
// Модуль включается переменной окружения SOS_MEMORY_PROFILE=<путь к CSV>.
// Без неё Create возвращает NULL, а в кадре остаётся одна проверка указателя.
// На Windows профилировщик перехватывает HeapAlloc/HeapFree/HeapReAlloc по
// IAT каждой загруженной laiue_*.dll и самой игры, ведёт таблицу
// указатель→(размер, модуль) и пишет строку CSV раз в секунду игрового
// времени. На остальных платформах модуль собран пустыми заглушками.

typedef struct SimulationMemoryProfile SimulationMemoryProfile;

// GPU/рендер и стриминг передаются примитивами, чтобы игровое ядро не
// зависело от render/scene SDK.

typedef struct SimulationMemoryProfileGpuStats
{
    uint64_t geometryPoolUsedBytes;
    uint64_t geometryPoolCapacityBytes;
    uint64_t uploadedBytes;
    uint64_t drawCalls;
    uint64_t drawnQuads;
    uint32_t scenePasses;
    uint64_t streamingQueuedRequests;
    uint64_t streamingCompletedBuilds;
    uint64_t streamingUploadedMeshes;
    uint32_t streamingPendingRequests;
    uint32_t streamingPendingResults;
    uint32_t streamingPeakUnfinishedWork;
    double streamingAverageBuildMilliseconds;
} SimulationMemoryProfileGpuStats;

// Читает SOS_MEMORY_PROFILE, открывает CSV и ставит хуки. NULL, если режим
// выключен или профилировщик создать не удалось.
SimulationMemoryProfile *SimulationMemoryProfileCreate(void);
void SimulationMemoryProfileDestroy(SimulationMemoryProfile *profile);

// Замер GPU-памяти, видимой в PrivateUsage: базовая точка до RendererCreate
// и конечная после готовности мира/стриминга. Разница PrivateUsage минус
// разница учтённых куч и есть оценка committed GPU/драйверных ресурсов,
// которые публичный API не отдаёт.
void SimulationMemoryProfileBeginGpuBaseline(SimulationMemoryProfile *profile);
void SimulationMemoryProfileEndGpuBaseline(SimulationMemoryProfile *profile);

// true, если с прошлой записи прошла секунда. Дешёвая проверка без побочных
// эффектов.
bool SimulationMemoryProfileDue(const SimulationMemoryProfile *profile, double nowSeconds);

// Пишет строку немедленно и запоминает момент. tag — метка строки
// ("window_create", "run", "final" и т. п.). gpu может быть NULL.
void SimulationMemoryProfileWrite(SimulationMemoryProfile *profile, double nowSeconds,
                                  uint64_t frameCount, uint32_t bodyCount, const char *tag,
                                  const SimulationMemoryProfileGpuStats *gpu);

// Уводит окно за пределы экрана в режиме замера: длинные прогоны не должны
// показывать окно на экране. Работает только на Windows.
void SimulationMemoryProfileHideWindow(void *nativeWindowHandle);

#endif
