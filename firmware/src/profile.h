// Профиль инвертора: какие регистры читать и как разложить их в состояние.
//
// Реализация выбирается на сборке: profile_eybond.cpp для первого инвертора
// (донгл Eybond, Modbus 9600, адрес 1) и profile_must.cpp для второго
// (Hiden Control HS35 = MUST PV18, USB, Modbus 19200, адрес 4).
#pragma once
#include <Arduino.h>

struct InvBlock { uint16_t start, count; };

extern const uint8_t   INV_SLAVE;        // адрес ведомого
extern const InvBlock  INV_BLOCKS[];     // блоки регистров для опроса по кругу
extern const uint8_t   INV_NBLOCKS;
extern const uint8_t   INV_PREFIX[];     // постоянный префикс ответа, если есть
extern const uint8_t   INV_PREFIX_LEN;
extern const char*     INV_PROFILE_NAME;

// Разложить прочитанный блок в снапшот состояния.
void invApplyBlock(uint16_t start, const uint8_t* words, uint8_t nwords);
