// Общий снапшот состояния. Единственный источник правды для веба и CLI.
// Пишет только задача BLE, читают все остальные — строго через snapshot().
#pragma once
#include <Arduino.h>
#include <functional>

static const uint8_t MAX_CELLS = 48;
static const uint8_t MAX_TEMPS = 8;

struct BmsState {
    // --- связь ---
    bool     linked        = false;  // BLE-соединение установлено
    bool     fresh         = false;  // данные не старше таймаута
    uint32_t lastFrameMs   = 0;      // millis() последнего валидного кадра
    uint32_t framesOk      = 0;
    uint32_t framesBad     = 0;      // не сошлась контрольная сумма
    uint32_t disconnects   = 0;
    int8_t   rssi          = 0;
    char     mac[18]       = {0};

    // --- 0x90 ---
    float    voltage       = 0;      // В
    float    current       = 0;      // А: + заряд, − разряд
    float    soc           = 0;      // %

    // --- 0x91 ---
    uint16_t cellMaxMv     = 0;
    uint16_t cellMinMv     = 0;
    uint8_t  cellMaxNo     = 0;
    uint8_t  cellMinNo     = 0;

    // --- 0x92 ---
    int16_t  tempMax       = 0;      // °C
    int16_t  tempMin       = 0;
    uint8_t  tempMaxNo     = 0;
    uint8_t  tempMinNo     = 0;

    // --- 0x93 ---
    uint8_t  chargeState   = 0;      // 0 покой, 1 заряд, 2 разряд
    bool     chargeMos     = false;
    bool     dischargeMos  = false;
    uint8_t  bmsLife       = 0;      // счётчик «жив», инкрементируется самой BMS
    uint32_t remainingMah  = 0;

    // --- 0x94 ---
    uint8_t  cellCount     = 0;
    uint8_t  tempCount     = 0;
    uint8_t  chargerOn     = 0;
    uint8_t  loadOn        = 0;

    // --- 0x95 / 0x96 / 0x97 / 0x98 ---
    uint16_t cellMv[MAX_CELLS]   = {0};
    int16_t  cellTemp[MAX_TEMPS] = {0};
    uint8_t  balance[6]          = {0};   // битовая маска балансировки, по ячейке на бит
    uint8_t  faults[8]           = {0};   // битовые маски аварий, DataID 0x98
    bool     haveFaults          = false;

    // --- производные ---
    float    power() const  { return voltage * current; }          // Вт
    uint16_t spreadMv() const {
        return (cellMaxMv > cellMinMv) ? (cellMaxMv - cellMinMv) : 0;
    }
};

// ------------------------------------------------------------------ инвертор
//
// Данных пока нет: связи по RS485 ещё нет, и до этапа разведки протокола
// структура наполняется только демо-режимом. Она заведена заранее, чтобы
// интерфейс, журнал и API были готовы принять их без переделки.

enum InvMode : uint8_t {
    INVM_UNKNOWN = 0,   // связи нет
    INVM_STANDBY,       // ожидание, выхода нет
    INVM_LINE,          // работа от сети
    INVM_BATTERY,       // работа от батареи и солнца
    INVM_BYPASS,        // обход: сеть напрямую на нагрузку
    INVM_FAULT,         // авария
};

struct InvState {
    bool     linked       = false;
    bool     fresh        = false;
    uint32_t lastFrameMs  = 0;

    uint8_t  mode         = INVM_UNKNOWN;
    bool     gridOn       = false;   // есть питающая сеть
    float    gridV        = 0;
    float    gridHz       = 0;

    float    outV         = 0;
    float    outHz        = 0;
    float    outW         = 0;       // мощность нагрузки
    uint8_t  loadPct      = 0;

    float    pvV          = 0;
    float    pvW          = 0;       // выработка солнечных панелей
    float    chgW         = 0;       // мощность заряда батареи от инвертора

    int16_t  tempC        = 0;
    uint8_t  faultCode    = 0;

    uint8_t  prioOut      = 0;       // приоритет источника выхода, программа 01
    uint8_t  prioChg      = 0;       // приоритет источника заряда, программа 16
};

void      stateInit();
BmsState  stateSnapshot();                 // копия под мьютексом
void      stateUpdate(const std::function<void(BmsState&)>& fn);  // изменение под мьютексом

InvState  invSnapshot();
void      invUpdate(const std::function<void(InvState&)>& fn);
