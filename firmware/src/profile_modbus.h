// Шаговый слой для профилей на Modbus: у первых двух инверторов он одинаков,
// различаются только адрес ведомого, набор блоков и разбор данных.
// Сама реализация — в profile_modbus.cpp, здесь только то, что должен
// определить конкретный профиль.
#pragma once
#include "profile.h"
#include "modbus.h"

struct InvBlock { uint16_t start, count; };

extern const uint8_t  INV_SLAVE;
extern const InvBlock INV_BLOCKS[];
extern const uint8_t  INV_PREFIX[];
extern const uint8_t  INV_PREFIX_LEN;

// Разложить прочитанный блок в снапшот — дело профиля.
void invApplyBlock(uint16_t start, const uint8_t* words, uint8_t nwords);
