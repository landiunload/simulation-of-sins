#ifndef SIMULATION_OF_SINS_GAME_GROUND_PROVIDER_H
#define SIMULATION_OF_SINS_GAME_GROUND_PROVIDER_H

#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

// Абсолютная высота пола. Пол — ровно один слой: снизу его всё равно не
// видно, а лишние слои дали бы мешеру работу без единого нового пикселя.
#define SIMULATION_GROUND_LEVEL INT64_C(0)

// Бесконечный пол как базовый слой мира.
//
// Заполнить блоками его нельзя — их бесконечно много, и никакой batch
// мутаций такого не выдержит. Базовый слой отвечает на вопрос «что здесь
// лежит» вычислением, поэтому пол существует всюду и не стоит ни байта
// памяти. Поверх него игра по-прежнему кладёт обычные блоки: sparse
// override движка главнее базового слоя.
typedef struct SimulationGroundProvider
{
    // Абсолютные координаты локального нуля. Rebasing двигает локальную
    // сетку, а пол обязан остаться на своей абсолютной высоте — иначе он
    // уезжал бы вместе с камерой.
    int64_t originBlock[3];
} SimulationGroundProvider;

void SimulationGroundProviderInit(SimulationGroundProvider *ground);

// Заполняет описание базового слоя. Контекст остаётся за приложением и
// обязан пережить World.
void SimulationGroundProviderBind(SimulationGroundProvider *ground,
                                  WorldBaseProvider *outProvider);

// Абсолютная высота, на которой сейчас лежит локальный ноль по Z.
int64_t SimulationGroundLocalLevel(const SimulationGroundProvider *ground);

#endif
