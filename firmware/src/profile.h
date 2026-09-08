// Профиль инвертора: что спрашивать и как понимать ответ.
//
// Первые два инвертора говорят Modbus RTU и различаются лишь скоростью,
// адресом и картой регистров. Третий говорит текстовым протоколом Voltronic,
// где регистров нет вовсе — есть команды вроде QPIGS и строка полей в ответе.
//
// Поэтому профиль описан не «блоками регистров», а шагами опроса: собрать
// запрос шага, скормить пришедшие байты, получить «ответ разобран». Что
// внутри — кадр Modbus или строка с пробелами — знает только сам профиль.
//
// Реализации выбираются на сборке:
//   profile_eybond.cpp    первый инвертор, Modbus через донгл Eybond
//   profile_must.cpp      второй, Hiden HS35 = MUST PV18, Modbus по USB
//   profile_voltronic.cpp третий, Asterion PLUS = Voltronic Axpert, ASCII
#pragma once
#include <Arduino.h>

extern const uint8_t INV_NSTEPS;        // сколько шагов в круге опроса
extern const char*   INV_PROFILE_NAME;

// Собрать запрос очередного шага. Возвращает длину; 0 — шаг пропускается.
size_t invBuildRequest(uint8_t step, uint8_t* out, size_t cap);

// Скормить пришедшие байты. true — ответ собран, разобран и уложен в снапшот.
bool   invFeed(uint8_t step, const uint8_t* data, size_t n);

// Сбросить накопленное: перед новым запросом и при обрыве связи.
void   invResetStream();
