#include "app/memory_profile.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)

#include <windows.h>

#include <intrin.h>
#include <psapi.h>
#include <tlhelp32.h>

// Открытая адресация фиксированной ёмкости: 1 Ми записей по 16 байт = 16 МиБ,
// выделенные VirtualAlloc вне перехватываемых куч. Переполнение — счётчик
// потерь, а не падение.
#define SIMULATION_MEMORY_TABLE_CAPACITY (1u << 20)
#define SIMULATION_MEMORY_TABLE_MASK (SIMULATION_MEMORY_TABLE_CAPACITY - 1u)
#define SIMULATION_MEMORY_TABLE_LIMIT \
    (SIMULATION_MEMORY_TABLE_CAPACITY - (SIMULATION_MEMORY_TABLE_CAPACITY / 8u))
#define SIMULATION_MEMORY_MAX_MODULES 64u
#define SIMULATION_MEMORY_MAX_NAME 64u
#define SIMULATION_MEMORY_NO_INDEX 0xFFFFFFFFu

typedef struct SimulationMemoryHeapEntry
{
    void *pointer;
    uint32_t size;
    uint32_t ownerPlusOne;
} SimulationMemoryHeapEntry;

typedef struct SimulationMemoryModule
{
    uintptr_t base;
    uintptr_t end;
    HMODULE handle;
    char name[SIMULATION_MEMORY_MAX_NAME];
    uint64_t liveBytes;
    uint64_t peakBytes;
    uint64_t allocationCount;
} SimulationMemoryModule;

typedef struct SimulationMemoryProfile
{
    FILE *file;
    SimulationMemoryHeapEntry *table;
    SRWLOCK lock;
    uint32_t capacity;
    uint32_t capacityMask;
    uint32_t capacityLimit;
    uint32_t moduleCount;
    SimulationMemoryModule modules[SIMULATION_MEMORY_MAX_MODULES];
    void *realHeapAlloc;
    void *realHeapReAlloc;
    void *realHeapFree;
    uint64_t liveBytes;
    uint64_t peakBytes;
    uint64_t trackedCount;
    uint64_t lostAllocations;
    uint64_t untrackedFrees;
    uint64_t tableBytes;
    uint64_t gpuVisibleBytes;
    uint64_t privateBaseline;
    uint64_t heapBaseline;
    bool gpuBaselineActive;
    bool hooksInstalled;
    double lastSampleSeconds;
    bool hasSample;
    LONG(NTAPI *ntQueryInformationThread)(HANDLE, LONG, PVOID, ULONG, PULONG);
} SimulationMemoryProfile;

// Полный ThreadBasicInformation (класс 0): ядро проверяет длину буфера и
// возвращает STATUS_INFO_LENGTH_MISMATCH на усечённой структуре.
typedef struct SimulationThreadBasicInformation
{
    LONG exitStatus;
    uint32_t padding;
    void *tebBaseAddress;
    void *uniqueProcess;
    void *uniqueThread;
    uintptr_t affinityMask;
    int32_t priority;
    int32_t basePriority;
} SimulationThreadBasicInformation;

static SimulationMemoryProfile *g_activeProfile;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static LPVOID WINAPI SimulationHeapAllocHook(HANDLE heap, DWORD flags, SIZE_T bytes);
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static LPVOID WINAPI SimulationHeapReAllocHook(HANDLE heap, DWORD flags, LPVOID pointer,
                                               SIZE_T bytes);
static BOOL WINAPI SimulationHeapFreeHook(HANDLE heap, DWORD flags, LPVOID pointer);

static uint32_t SimulationMemoryHash(const void *pointer)
{
    uintptr_t value = (uintptr_t)pointer;
    value >>= 4;
    value *= (uintptr_t)UINT64_C(0x9E3779B97F4A7C15);
    return (uint32_t)(value >> 32);
}

static uint64_t SimulationReadPrivateUsage(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters,
                                (DWORD)sizeof(counters)) == FALSE)
    {
        return 0u;
    }
    return (uint64_t)counters.PrivateUsage;
}

static void SimulationWideToNarrow(const wchar_t *source, char *destination, size_t capacity)
{
    size_t index = 0u;
    for (; source[index] != L'\0' && index + 1u < capacity; ++index)
    {
        wchar_t character = source[index];
        destination[index] = character < 128 ? (char)character : '?';
    }
    destination[index] = '\0';
}

static void SimulationCopyName(const char *source, char *destination, size_t capacity)
{
    size_t written = 0u;
    for (size_t index = 0u; source[index] != '\0' && written + 1u < capacity; ++index)
    {
        char character = source[index];
        if (character == '.')
        {
            break;
        }
        if (character >= 'A' && character <= 'Z')
        {
            character = (char)(character - 'A' + 'a');
        }
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9')))
        {
            character = '_';
        }
        destination[written++] = character;
    }
    destination[written] = '\0';
}

static bool SimulationIsTargetModule(const char *name, uintptr_t base)
{
    if (base == (uintptr_t)GetModuleHandleW(NULL))
    {
        return true;
    }
    const char *prefix = "laiue_";
    for (size_t index = 0u; prefix[index] != '\0'; ++index)
    {
        char character = name[index];
        if (character >= 'A' && character <= 'Z')
        {
            character = (char)(character - 'A' + 'a');
        }
        if (character != prefix[index])
        {
            return false;
        }
    }
    return true;
}

// Собирает адреса и имена модулей, чью кучу считаем. Индекса 0 нет: запись
// ownerPlusOne хранит индекс+1, а 0 означает пустой слот таблицы.
static void SimulationCollectModules(SimulationMemoryProfile *profile)
{
    profile->modules[0].base = 0u;
    profile->modules[0].end = 0u;
    profile->modules[0].handle = NULL;
    SimulationCopyName("unknown", profile->modules[0].name, SIMULATION_MEMORY_MAX_NAME);
    profile->moduleCount = 1u;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }
    MODULEENTRY32W entry;
    entry.dwSize = (DWORD)sizeof(entry);
    if (Module32FirstW(snapshot, &entry) != FALSE)
    {
        do
        {
            if (profile->moduleCount >= SIMULATION_MEMORY_MAX_MODULES)
            {
                break;
            }
            char narrowName[SIMULATION_MEMORY_MAX_NAME];
            SimulationWideToNarrow(entry.szModule, narrowName, sizeof(narrowName));
            if (!SimulationIsTargetModule(narrowName, (uintptr_t)entry.modBaseAddr))
            {
                continue;
            }
            SimulationMemoryModule *module = &profile->modules[profile->moduleCount];
            module->base = (uintptr_t)entry.modBaseAddr;
            module->end = module->base + (uintptr_t)entry.modBaseSize;
            module->handle = entry.hModule;
            module->liveBytes = 0u;
            module->peakBytes = 0u;
            module->allocationCount = 0u;
            SimulationCopyName(narrowName, module->name, SIMULATION_MEMORY_MAX_NAME);
            profile->moduleCount++;
        } while (Module32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void SimulationPatchIatSlot(void *hook, void *destination)
{
    void **slot = (void **)destination;
    if (*slot == hook)
    {
        return;
    }
    DWORD protection = 0u;
    if (VirtualProtect((LPVOID)slot, sizeof(*slot), PAGE_READWRITE, &protection) == FALSE)
    {
        return;
    }
    *slot = hook;
    DWORD ignored = 0u;
    (void)VirtualProtect((LPVOID)slot, sizeof(*slot), protection, &ignored);
}

static void SimulationInstallModuleHooks(SimulationMemoryProfile *profile, HMODULE module)
{
    const unsigned char *base = (const unsigned char *)module;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return;
    }
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return;
    }
    const IMAGE_DATA_DIRECTORY *directory =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory->VirtualAddress == 0u)
    {
        return;
    }
    const IMAGE_IMPORT_DESCRIPTOR *descriptor =
        (const IMAGE_IMPORT_DESCRIPTOR *)(base + directory->VirtualAddress);
    for (; descriptor->Name != 0u; ++descriptor)
    {
        const IMAGE_THUNK_DATA *lookup = (const IMAGE_THUNK_DATA *)(base +
            (descriptor->OriginalFirstThunk != 0u ? descriptor->OriginalFirstThunk
                                                  : descriptor->FirstThunk));
        IMAGE_THUNK_DATA *address = (IMAGE_THUNK_DATA *)(base + descriptor->FirstThunk);
        for (; lookup->u1.Function != 0u; ++lookup, ++address)
        {
            if ((lookup->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0u)
            {
                continue;
            }
            const IMAGE_IMPORT_BY_NAME *imported =
                (const IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
            const char *name = (const char *)imported->Name;
            void **destination = (void **)&address->u1.Function;
            if (strcmp(name, "HeapAlloc") == 0)
            {
                if (profile->realHeapAlloc == NULL)
                {
                    profile->realHeapAlloc = *destination;
                }
                SimulationPatchIatSlot((void *)&SimulationHeapAllocHook, (void *)destination);
            }
            else if (strcmp(name, "HeapReAlloc") == 0)
            {
                if (profile->realHeapReAlloc == NULL)
                {
                    profile->realHeapReAlloc = *destination;
                }
                SimulationPatchIatSlot((void *)&SimulationHeapReAllocHook, (void *)destination);
            }
            else if (strcmp(name, "HeapFree") == 0)
            {
                if (profile->realHeapFree == NULL)
                {
                    profile->realHeapFree = *destination;
                }
                SimulationPatchIatSlot((void *)&SimulationHeapFreeHook, (void *)destination);
            }
        }
    }
}

static void SimulationRestoreModuleHooks(SimulationMemoryProfile *profile, HMODULE module)
{
    const unsigned char *base = (const unsigned char *)module;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return;
    }
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return;
    }
    const IMAGE_DATA_DIRECTORY *directory =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory->VirtualAddress == 0u)
    {
        return;
    }
    const IMAGE_IMPORT_DESCRIPTOR *descriptor =
        (const IMAGE_IMPORT_DESCRIPTOR *)(base + directory->VirtualAddress);
    for (; descriptor->Name != 0u; ++descriptor)
    {
        const IMAGE_THUNK_DATA *lookup = (const IMAGE_THUNK_DATA *)(base +
            (descriptor->OriginalFirstThunk != 0u ? descriptor->OriginalFirstThunk
                                                  : descriptor->FirstThunk));
        IMAGE_THUNK_DATA *address = (IMAGE_THUNK_DATA *)(base + descriptor->FirstThunk);
        for (; lookup->u1.Function != 0u; ++lookup, ++address)
        {
            if ((lookup->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0u)
            {
                continue;
            }
            const IMAGE_IMPORT_BY_NAME *imported =
                (const IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
            const char *name = (const char *)imported->Name;
            void *hook = NULL;
            void *real = NULL;
            if (strcmp(name, "HeapAlloc") == 0)
            {
                hook = (void *)&SimulationHeapAllocHook;
                real = profile->realHeapAlloc;
            }
            else if (strcmp(name, "HeapReAlloc") == 0)
            {
                hook = (void *)&SimulationHeapReAllocHook;
                real = profile->realHeapReAlloc;
            }
            else if (strcmp(name, "HeapFree") == 0)
            {
                hook = (void *)&SimulationHeapFreeHook;
                real = profile->realHeapFree;
            }
            if (hook == NULL || real == NULL)
            {
                continue;
            }
            void **slot = (void **)&address->u1.Function;
            if (*slot != hook)
            {
                continue;
            }
            DWORD protection = 0u;
            if (VirtualProtect((LPVOID)slot, sizeof(*slot), PAGE_READWRITE, &protection) ==
                FALSE)
            {
                continue;
            }
            *slot = real;
            DWORD ignored = 0u;
            (void)VirtualProtect((LPVOID)slot, sizeof(*slot), protection, &ignored);
        }
    }
}

static void SimulationInstallHooks(SimulationMemoryProfile *profile)
{
    for (uint32_t index = 1u; index < profile->moduleCount; ++index)
    {
        if (profile->modules[index].handle != NULL)
        {
            SimulationInstallModuleHooks(profile, profile->modules[index].handle);
        }
    }
    profile->hooksInstalled = profile->realHeapAlloc != NULL && profile->realHeapFree != NULL;
}

static void SimulationRestoreHooks(SimulationMemoryProfile *profile)
{
    if (!profile->hooksInstalled)
    {
        return;
    }
    for (uint32_t index = 1u; index < profile->moduleCount; ++index)
    {
        if (profile->modules[index].handle != NULL)
        {
            SimulationRestoreModuleHooks(profile, profile->modules[index].handle);
        }
    }
    profile->hooksInstalled = false;
}

static uint32_t SimulationModuleFromAddress(const SimulationMemoryProfile *profile,
                                            const void *address)
{
    uintptr_t value = (uintptr_t)address;
    for (uint32_t index = 1u; index < profile->moduleCount; ++index)
    {
        if (value >= profile->modules[index].base && value < profile->modules[index].end)
        {
            return index;
        }
    }
    return 0u;
}

// Возвращает true, если запись действительно вставлена. Весь проход под
// внешним SRW-замком; внутри хуков никаких аллокаций.
static bool SimulationMemoryHeapInsert(SimulationMemoryProfile *profile, void *pointer,
                                       // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                       uint32_t size, uint32_t ownerPlusOne)
{
    if (profile->trackedCount >= profile->capacityLimit)
    {
        profile->lostAllocations++;
        return false;
    }
    uint32_t index = SimulationMemoryHash(pointer) & profile->capacityMask;
    for (;;)
    {
        SimulationMemoryHeapEntry *entry = &profile->table[index];
        if (entry->ownerPlusOne == 0u)
        {
            entry->pointer = pointer;
            entry->size = size;
            entry->ownerPlusOne = ownerPlusOne;
            break;
        }
        if (entry->pointer == pointer)
        {
            return false;
        }
        index = (index + 1u) & profile->capacityMask;
    }

    profile->trackedCount++;
    profile->liveBytes += size;
    if (profile->liveBytes > profile->peakBytes)
    {
        profile->peakBytes = profile->liveBytes;
    }
    SimulationMemoryModule *module = &profile->modules[ownerPlusOne - 1u];
    module->liveBytes += size;
    module->allocationCount++;
    if (module->liveBytes > module->peakBytes)
    {
        module->peakBytes = module->liveBytes;
    }
    return true;
}

static uint32_t SimulationMemoryHeapFind(const SimulationMemoryProfile *profile,
                                         const void *pointer)
{
    uint32_t index = SimulationMemoryHash(pointer) & profile->capacityMask;
    while (profile->table[index].ownerPlusOne != 0u)
    {
        if (profile->table[index].pointer == pointer)
        {
            return index;
        }
        index = (index + 1u) & profile->capacityMask;
    }
    return SIMULATION_MEMORY_NO_INDEX;
}

// Обратный сдвиг кластера: таблица остаётся без надгробий, поэтому пустой
// слот по-прежнему означает конец цепочки пробирования.
static void SimulationMemoryHeapErase(SimulationMemoryProfile *profile, uint32_t hole)
{
    uint32_t slot = hole;
    for (;;)
    {
        profile->table[slot].pointer = NULL;
        profile->table[slot].size = 0u;
        profile->table[slot].ownerPlusOne = 0u;
        uint32_t probe = slot;
        for (;;)
        {
            probe = (probe + 1u) & profile->capacityMask;
            if (profile->table[probe].ownerPlusOne == 0u)
            {
                return;
            }
            uint32_t home = SimulationMemoryHash(profile->table[probe].pointer) &
                            profile->capacityMask;
            uint32_t distanceHome = (home - slot) & profile->capacityMask;
            uint32_t distanceProbe = (probe - slot) & profile->capacityMask;
            if (distanceHome != 0u && distanceHome <= distanceProbe)
            {
                break;
            }
        }
        profile->table[slot] = profile->table[probe];
        slot = probe;
    }
}

static void SimulationMemoryHeapTrack(SimulationMemoryProfile *profile, void *pointer,
                                      SIZE_T bytes, void *caller)
{
    if (pointer == NULL)
    {
        return;
    }
    uint32_t size = bytes > (SIZE_T)UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
    uint32_t owner = SimulationModuleFromAddress(profile, caller);
    AcquireSRWLockExclusive(&profile->lock);
    (void)SimulationMemoryHeapInsert(profile, pointer, size, owner + 1u);
    ReleaseSRWLockExclusive(&profile->lock);
}

static int32_t SimulationMemoryHeapUntrack(SimulationMemoryProfile *profile, void *pointer)
{
    if (pointer == NULL)
    {
        return -1;
    }
    AcquireSRWLockExclusive(&profile->lock);
    uint32_t index = SimulationMemoryHeapFind(profile, pointer);
    if (index == SIMULATION_MEMORY_NO_INDEX)
    {
        profile->untrackedFrees++;
        ReleaseSRWLockExclusive(&profile->lock);
        return -1;
    }
    SimulationMemoryHeapEntry entry = profile->table[index];
    profile->liveBytes -= entry.size;
    profile->trackedCount--;
    SimulationMemoryModule *module = &profile->modules[entry.ownerPlusOne - 1u];
    module->liveBytes -= entry.size;
    SimulationMemoryHeapErase(profile, index);
    ReleaseSRWLockExclusive(&profile->lock);
    return (int32_t)(entry.ownerPlusOne - 1u);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static LPVOID WINAPI SimulationHeapAllocHook(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    SimulationMemoryProfile *profile = g_activeProfile;
    LPVOID result =
        ((LPVOID(WINAPI *)(HANDLE, DWORD, SIZE_T))profile->realHeapAlloc)(heap, flags, bytes);
    if (result != NULL)
    {
        SimulationMemoryHeapTrack(profile, result, bytes, _ReturnAddress());
    }
    return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static LPVOID WINAPI SimulationHeapReAllocHook(HANDLE heap, DWORD flags, LPVOID pointer,
                                               SIZE_T bytes)
{
    SimulationMemoryProfile *profile = g_activeProfile;
    int32_t previousOwner = -1;
    if (pointer != NULL)
    {
        previousOwner = SimulationMemoryHeapUntrack(profile, pointer);
    }
    void *caller = _ReturnAddress();
    LPVOID result =
        ((LPVOID(WINAPI *)(HANDLE, DWORD, LPVOID, SIZE_T))profile->realHeapReAlloc)(
            heap, flags, pointer, bytes);
    if (result != NULL)
    {
        if (previousOwner >= 0)
        {
            uint32_t size = bytes > (SIZE_T)UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
            AcquireSRWLockExclusive(&profile->lock);
            (void)SimulationMemoryHeapInsert(profile, result, size,
                                             (uint32_t)previousOwner + 1u);
            ReleaseSRWLockExclusive(&profile->lock);
        }
        else
        {
            SimulationMemoryHeapTrack(profile, result, bytes, caller);
        }
    }
    return result;
}

static BOOL WINAPI SimulationHeapFreeHook(HANDLE heap, DWORD flags, LPVOID pointer)
{
    SimulationMemoryProfile *profile = g_activeProfile;
    if (pointer != NULL)
    {
        (void)SimulationMemoryHeapUntrack(profile, pointer);
    }
    return ((BOOL(WINAPI *)(HANDLE, DWORD, LPVOID))profile->realHeapFree)(heap, flags, pointer);
}

// Границы стека различаются по смыслу, но не по типу; перестановка даст
// nonsense-диапазон.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static uint64_t SimulationStackCommitted(const void *stackBase, const void *stackLimit)
{
    uint64_t committed = 0u;
    const unsigned char *address = (const unsigned char *)stackLimit;
    const unsigned char *end = (const unsigned char *)stackBase;
    while (address < end)
    {
        MEMORY_BASIC_INFORMATION information;
        if (VirtualQuery((LPCVOID)address, &information, sizeof(information)) == 0u)
        {
            break;
        }
        const unsigned char *regionEnd =
            (const unsigned char *)information.BaseAddress + information.RegionSize;
        if (information.State == MEM_COMMIT)
        {
            committed += (uint64_t)information.RegionSize;
        }
        if (regionEnd <= address)
        {
            break;
        }
        address = regionEnd;
    }
    return committed;
}

static uint32_t SimulationCountThreads(const SimulationMemoryProfile *profile,
                                       uint64_t *outCommittedBytes)
{
    *outCommittedBytes = 0u;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0u);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0u;
    }
    uint32_t count = 0u;
    uint64_t committed = 0u;
    DWORD processId = GetCurrentProcessId();
    THREADENTRY32 entry;
    entry.dwSize = (DWORD)sizeof(entry);
    if (Thread32First(snapshot, &entry) != FALSE)
    {
        do
        {
            if (entry.th32OwnerProcessID != processId)
            {
                continue;
            }
            count++;
            if (profile->ntQueryInformationThread == NULL)
            {
                continue;
            }
            HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == NULL)
            {
                continue;
            }
            SimulationThreadBasicInformation information;
            ULONG returned = 0u;
            if (profile->ntQueryInformationThread(thread, 0, &information,
                                                  (ULONG)sizeof(information),
                                                  &returned) >= 0 &&
                information.tebBaseAddress != NULL)
            {
                const NT_TIB *tib = (const NT_TIB *)information.tebBaseAddress;
                committed += SimulationStackCommitted(tib->StackBase, tib->StackLimit);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    *outCommittedBytes = committed;
    return count;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void SimulationSumModuleImages(uint64_t *outAllBytes, uint64_t *outEngineBytes)
{
    *outAllBytes = 0u;
    *outEngineBytes = 0u;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }
    uint64_t allBytes = 0u;
    uint64_t engineBytes = 0u;
    MODULEENTRY32W entry;
    entry.dwSize = (DWORD)sizeof(entry);
    if (Module32FirstW(snapshot, &entry) != FALSE)
    {
        do
        {
            allBytes += (uint64_t)entry.modBaseSize;
            char narrowName[SIMULATION_MEMORY_MAX_NAME];
            SimulationWideToNarrow(entry.szModule, narrowName, sizeof(narrowName));
            if (SimulationIsTargetModule(narrowName, (uintptr_t)entry.modBaseAddr))
            {
                engineBytes += (uint64_t)entry.modBaseSize;
            }
        } while (Module32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    *outAllBytes = allBytes;
    *outEngineBytes = engineBytes;
}

static void SimulationWriteHeader(SimulationMemoryProfile *profile)
{
    fputs("tag,time_seconds,frame_count,bodies,"
          "private_usage_bytes,working_set_bytes,peak_working_set_bytes,"
          "pagefile_usage_bytes,handle_count,thread_count,"
          "thread_stack_reserved_upper_bytes,thread_stack_committed_bytes,"
          "module_images_all_bytes,module_images_engine_bytes,"
          "heap_live_bytes,heap_peak_bytes,heap_tracked_count,"
          "heap_lost_allocations,heap_untracked_frees,"
          "geometry_pool_used_bytes,geometry_pool_capacity_bytes,"
          "uploaded_bytes,draw_calls,drawn_quads,scene_passes,"
          "streaming_queued,streaming_completed,streaming_uploaded,"
          "streaming_pending_requests,streaming_pending_results,"
          "streaming_peak_unfinished,streaming_avg_build_ms,"
          "gpu_visible_bytes,profiler_table_bytes,"
          "accounted_bytes,unaccounted_bytes,unaccounted_percent",
          profile->file);
    for (uint32_t index = 0u; index < profile->moduleCount; ++index)
    {
        (void)fprintf_s(profile->file, ",%s_live_bytes,%s_peak_bytes,%s_allocations",
                      profile->modules[index].name, profile->modules[index].name,
                      profile->modules[index].name);
    }
    fputc('\n', profile->file);
    fflush(profile->file);
}

// Параметр tags/gpu не переставляется: их роль задана форматом CSV.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void SimulationMemoryProfileWrite(SimulationMemoryProfile *profile, double nowSeconds,
                                  uint64_t frameCount, uint32_t bodyCount, const char *tag,
                                  const SimulationMemoryProfileGpuStats *gpu)
{
    if (profile == NULL || profile->file == NULL)
    {
        return;
    }
    PROCESS_MEMORY_COUNTERS_EX counters;
    uint64_t privateUsage = 0u;
    uint64_t workingSet = 0u;
    uint64_t peakWorkingSet = 0u;
    uint64_t pagefileUsage = 0u;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters,
                                (DWORD)sizeof(counters)) != FALSE)
    {
        privateUsage = (uint64_t)counters.PrivateUsage;
        workingSet = (uint64_t)counters.WorkingSetSize;
        peakWorkingSet = (uint64_t)counters.PeakWorkingSetSize;
        pagefileUsage = (uint64_t)counters.PagefileUsage;
    }
    DWORD handleCount = 0u;
    (void)GetProcessHandleCount(GetCurrentProcess(), &handleCount);
    uint64_t stackCommitted = 0u;
    uint32_t threadCount = SimulationCountThreads(profile, &stackCommitted);
    uint64_t imageAll = 0u;
    uint64_t imageEngine = 0u;
    SimulationSumModuleImages(&imageAll, &imageEngine);

    // Копируются только счётчики, не весь массив модулей: стек функции
    // обязан укладываться в 4 КиБ, а имена модулей неизменны после Create и
    // читаются без замка.
    uint64_t moduleLive[SIMULATION_MEMORY_MAX_MODULES];
    uint64_t modulePeak[SIMULATION_MEMORY_MAX_MODULES];
    uint64_t moduleAllocations[SIMULATION_MEMORY_MAX_MODULES];
    uint64_t liveBytes = 0u;
    uint64_t peakBytes = 0u;
    uint64_t trackedCount = 0u;
    uint64_t lostAllocations = 0u;
    uint64_t untrackedFrees = 0u;
    uint32_t moduleCount = 0u;
    AcquireSRWLockExclusive(&profile->lock);
    liveBytes = profile->liveBytes;
    peakBytes = profile->peakBytes;
    trackedCount = profile->trackedCount;
    lostAllocations = profile->lostAllocations;
    untrackedFrees = profile->untrackedFrees;
    moduleCount = profile->moduleCount;
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        moduleLive[index] = profile->modules[index].liveBytes;
        modulePeak[index] = profile->modules[index].peakBytes;
        moduleAllocations[index] = profile->modules[index].allocationCount;
    }
    ReleaseSRWLockExclusive(&profile->lock);

    SimulationMemoryProfileGpuStats zeroGpu;
    if (gpu == NULL)
    {
        zeroGpu = (SimulationMemoryProfileGpuStats){0};
        gpu = &zeroGpu;
    }

    uint64_t gpuVisible = profile->gpuVisibleBytes;
    uint64_t accounted = liveBytes + gpuVisible;
    long long unaccounted = (long long)privateUsage - (long long)accounted;
    double unaccountedPercent =
        privateUsage == 0u ? 0.0 : (double)unaccounted * 100.0 / (double)privateUsage;

    (void)fprintf_s(
        profile->file,
        "%s,%.6f,%llu,%u,"
        "%llu,%llu,%llu,%llu,%lu,%u,"
        "%llu,%llu,"
        "%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%u,"
        "%llu,%llu,%llu,%u,%u,%u,%.6f,"
        "%llu,%llu,"
        "%llu,%lld,%.3f",
        tag, nowSeconds, (unsigned long long)frameCount, bodyCount, (unsigned long long)privateUsage,
        (unsigned long long)workingSet, (unsigned long long)peakWorkingSet,
        (unsigned long long)pagefileUsage, (unsigned long)handleCount, threadCount,
        (unsigned long long)((uint64_t)threadCount * 1048576u),
        (unsigned long long)stackCommitted, (unsigned long long)imageAll,
        (unsigned long long)imageEngine, (unsigned long long)liveBytes,
        (unsigned long long)peakBytes, (unsigned long long)trackedCount,
        (unsigned long long)lostAllocations, (unsigned long long)untrackedFrees,
        (unsigned long long)gpu->geometryPoolUsedBytes,
        (unsigned long long)gpu->geometryPoolCapacityBytes,
        (unsigned long long)gpu->uploadedBytes, (unsigned long long)gpu->drawCalls,
        (unsigned long long)gpu->drawnQuads, gpu->scenePasses,
        (unsigned long long)gpu->streamingQueuedRequests,
        (unsigned long long)gpu->streamingCompletedBuilds,
        (unsigned long long)gpu->streamingUploadedMeshes, gpu->streamingPendingRequests,
        gpu->streamingPendingResults, gpu->streamingPeakUnfinishedWork,
        gpu->streamingAverageBuildMilliseconds, (unsigned long long)gpuVisible,
        (unsigned long long)profile->tableBytes, (unsigned long long)accounted, unaccounted,
        unaccountedPercent);
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        (void)fprintf_s(profile->file, ",%llu,%llu,%llu",
                        (unsigned long long)moduleLive[index],
                        (unsigned long long)modulePeak[index],
                        (unsigned long long)moduleAllocations[index]);
    }
    fputc('\n', profile->file);
    fflush(profile->file);
    profile->lastSampleSeconds = nowSeconds;
    profile->hasSample = true;
}

bool SimulationMemoryProfileDue(const SimulationMemoryProfile *profile, double nowSeconds)
{
    if (profile == NULL)
    {
        return false;
    }
    if (!profile->hasSample)
    {
        return true;
    }
    return nowSeconds - profile->lastSampleSeconds >= 1.0;
}

void SimulationMemoryProfileBeginGpuBaseline(SimulationMemoryProfile *profile)
{
    if (profile == NULL)
    {
        return;
    }
    profile->privateBaseline = SimulationReadPrivateUsage();
    profile->heapBaseline = profile->liveBytes;
    profile->gpuBaselineActive = true;
}

void SimulationMemoryProfileEndGpuBaseline(SimulationMemoryProfile *profile)
{
    if (profile == NULL || !profile->gpuBaselineActive)
    {
        return;
    }
    uint64_t privateUsage = SimulationReadPrivateUsage();
    uint64_t heapLive = profile->liveBytes;
    long long privateDelta = (long long)privateUsage - (long long)profile->privateBaseline;
    long long heapDelta = (long long)heapLive - (long long)profile->heapBaseline;
    long long gpuBytes = privateDelta - heapDelta;
    if (gpuBytes < 0)
    {
        gpuBytes = 0;
    }
    profile->gpuVisibleBytes = (uint64_t)gpuBytes;
    profile->gpuBaselineActive = false;
}

void SimulationMemoryProfileHideWindow(void *nativeWindowHandle)
{
    HWND window = (HWND)nativeWindowHandle;
    if (window == NULL)
    {
        return;
    }
    (void)SetWindowPos(window, NULL, -32000, -32000, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

SimulationMemoryProfile *SimulationMemoryProfileCreate(void)
{
    char *ownedPath = NULL;
    size_t ownedBytes = 0u;
    (void)_dupenv_s(&ownedPath, &ownedBytes, "SOS_MEMORY_PROFILE");
    const char *path = ownedPath;
    if (path == NULL || path[0] == '\0')
    {
        free(ownedPath);
        return NULL;
    }
    FILE *file = NULL;
    (void)fopen_s(&file, path, "wb");
    free(ownedPath);
    if (file == NULL)
    {
        return NULL;
    }
    SimulationMemoryProfile *profile =
        (SimulationMemoryProfile *)calloc(1u, sizeof(SimulationMemoryProfile));
    if (profile == NULL)
    {
        fclose(file);
        return NULL;
    }
    profile->file = file;
    profile->capacity = SIMULATION_MEMORY_TABLE_CAPACITY;
    profile->capacityMask = SIMULATION_MEMORY_TABLE_MASK;
    profile->capacityLimit = SIMULATION_MEMORY_TABLE_LIMIT;
    InitializeSRWLock(&profile->lock);
    profile->tableBytes = (uint64_t)profile->capacity * sizeof(SimulationMemoryHeapEntry);
    profile->table = (SimulationMemoryHeapEntry *)VirtualAlloc(
        NULL, (SIZE_T)profile->tableBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (profile->table == NULL)
    {
        fclose(file);
        free(profile);
        return NULL;
    }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != NULL)
    {
        // UNION вместо каста FARPROC → указатель на функцию: каст через
        // void* — ошибка bugprone-casting-through-void, типы у функций разные.
        union
        {
            FARPROC address;
            LONG(NTAPI *function)(HANDLE, LONG, PVOID, ULONG, PULONG);
        } resolved;
        resolved.address = GetProcAddress(ntdll, "NtQueryInformationThread");
        profile->ntQueryInformationThread = resolved.function;
    }
    SimulationCollectModules(profile);
    SimulationWriteHeader(profile);
    g_activeProfile = profile;
    SimulationInstallHooks(profile);
    if (!profile->hooksInstalled)
    {
        // Кучи не перехвачены (не найден импорт): не врём нулями, а сразу
        // сообщаем об этом строкой. Режим остаётся, шапка уже записана.
        profile->lostAllocations = 0u;
    }
    return profile;
}

void SimulationMemoryProfileDestroy(SimulationMemoryProfile *profile)
{
    if (profile == NULL)
    {
        return;
    }
    if (profile->file != NULL)
    {
        fclose(profile->file);
        profile->file = NULL;
    }
    // Хуки снимаются только когда вызывающая сторона гарантировала, что
    // рабочих потоков движка больше нет (см. application.c). После возврата
    // IAT снова указывает на настоящие HeapAlloc/HeapFree/HeapReAlloc.
    SimulationRestoreHooks(profile);
    if (g_activeProfile == profile)
    {
        g_activeProfile = NULL;
    }
    if (profile->table != NULL)
    {
        (void)VirtualFree(profile->table, 0u, MEM_RELEASE);
        profile->table = NULL;
    }
    free(profile);
}

#else

struct SimulationMemoryProfile
{
    int unused;
};

SimulationMemoryProfile *SimulationMemoryProfileCreate(void)
{
    return NULL;
}

void SimulationMemoryProfileDestroy(SimulationMemoryProfile *profile)
{
    (void)profile;
}

void SimulationMemoryProfileBeginGpuBaseline(SimulationMemoryProfile *profile)
{
    (void)profile;
}

void SimulationMemoryProfileEndGpuBaseline(SimulationMemoryProfile *profile)
{
    (void)profile;
}

bool SimulationMemoryProfileDue(const SimulationMemoryProfile *profile, double nowSeconds)
{
    (void)profile;
    (void)nowSeconds;
    return false;
}

void SimulationMemoryProfileWrite(SimulationMemoryProfile *profile, double nowSeconds,
                                  uint64_t frameCount, uint32_t bodyCount, const char *tag,
                                  const SimulationMemoryProfileGpuStats *gpu)
{
    (void)profile;
    (void)nowSeconds;
    (void)frameCount;
    (void)bodyCount;
    (void)tag;
    (void)gpu;
}

void SimulationMemoryProfileHideWindow(void *nativeWindowHandle)
{
    (void)nativeWindowHandle;
}

#endif
