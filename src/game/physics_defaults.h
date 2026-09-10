#ifndef SIMULATION_OF_SINS_GAME_PHYSICS_DEFAULTS_H
#define SIMULATION_OF_SINS_GAME_PHYSICS_DEFAULTS_H

#include <stdbool.h>
#include <stdint.h>

// Пул включает вызывающий поток, поэтому это число потоков всего, а не
// «рабочих сверх главного». Замер на восьмиядерной машине показал, что
// сеточный отбор ускоряется до шести потоков, а заполнение всех ядер даёт
// нестабильный результат; запас ядер нужен рендеру и системе.
#define SIMULATION_PHYSICS_DEFAULT_THREAD_CAP 6u

// Строгий разбор SOS_PHYSICS_THREADS: только десятичные цифры, 1..64, без
// знака, пробелов и хвостов. false (и *outThreads не тронут) для NULL,
// пустой строки и недопустимого значения.
bool SimulationParsePhysicsThreads(const char *text, uint32_t *outThreads);

// Потоки по умолчанию, когда окружение не переопределяет: не меньше одного,
// не больше потолка и не больше доступных логических процессоров.
uint32_t SimulationDefaultPhysicsThreads(uint32_t logicalProcessorCount);

// Эффективное число потоков: разобранное переопределение, иначе умолчание.
uint32_t SimulationPhysicsThreads(const char *text, uint32_t logicalProcessorCount);

// Выбор широкого отбора: дерево включает только точное "tree". Всё прочее
// (включая отсутствие переменной) оставляет сетку — она измеренно быстрее
// для равномерного спавнера кубов.
bool SimulationUseSpatialIndex(const char *text);

#endif
