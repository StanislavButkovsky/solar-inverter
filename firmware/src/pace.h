// BMS PACE напрямую по RS485: ячейки, температуры, балансировка, защиты.
//
// У HS35 батарея говорит с инвертором по CAN, и через инвертор (GBMS) идёт
// только общее: напряжение, ток, заряд. Ячеек там нет. Батареи владельца —
// две ENSSE ENS-Li48V100 (48 В, 100 А·ч), панель как у BMS PACE: свободный
// порт RS485 рядом с CAN. Модуль RS485↔TTL на 3,3 В с автоматическим
// направлением: RO → GPIO 18, DI ← GPIO 17. RJ45 (по esphome-pace-bms):
// 1 B−, 2 A+, 3 GND.
//
// Протокол модуль определяет сам, перебором при первом включении:
//   * PACE ASCII 0x25 («~25…»): CID2 0x42 — аналоговые, 0x44 — состояние;
//   * PACE Modbus RTU: регистры 0–36 (ток, напряжение, ёмкости, циклы,
//     флаги, ячейки, температуры);
//   * PACE ASCII 0x20 — старые прошивки и режим «как Pylontech».
// Всё на 9600 8N1, адреса 0–15 — DIP-переключатели «АДР.». Нашёл одну
// батарею — ищет и остальные тем же протоколом. Найденное — в NVS.
// Ответ, который не разобрался, сохраняется сырым — по нему дописывается.
//
// Источники: nkinnan/esphome-pace-bms (протоколы 0x20/0x25),
// «PACE BMS Modbus Protocol for RS485 V1.3» (2017).
#pragma once
#include <Arduino.h>

void   paceStart();
String paceJson();             // для /api/pace и страницы
String paceText();             // для бота: /cells
String paceAlarm();            // защиты и неисправности всех батарей словами; "" — нет

// Разбор кадров — отдельно, чтобы проверять на компьютере на чужих кадрах.
struct PacePack {
    bool     ok = false;
    uint8_t  addr = 0;
    uint8_t  cells = 0;
    uint16_t mv[16] = { 0 };
    uint8_t  temps = 0;
    int16_t  t10[8] = { 0 };       // 0,1 °C
    float    curA = 0, voltV = 0;
    float    remAh = 0, fullAh = 0, designAh = 0;
    uint16_t cycles = 0;
    float    soh = -1;             // %, −1 — нет
    // состояние
    bool     stOk = false;
    uint16_t balance = 0;          // бит на ячейку
    uint16_t warn = 0, prot = 0, fault = 0;   // в разметке Modbus (см. pace.cpp)
    bool     chgMos = false, dsgMos = false;
};

// ASCII: кадр «~VVAA46CCLLLL…SSSS\r». Вернёт длину запроса.
size_t paceAsciiRequest(char* out, size_t cap, uint8_t ver, uint8_t addr, uint8_t cid2);
bool   paceAsciiCheck(const char* frame, size_t n);                 // SOI, EOI, длина, сумма
bool   paceAsciiAnalog(const char* frame, size_t n, PacePack& p);   // ответ на 0x42
bool   paceAsciiStatus(const char* frame, size_t n, PacePack& p);   // ответ на 0x44
// Modbus RTU.
size_t paceModbusRequest(uint8_t* out, uint8_t addr, uint16_t reg, uint16_t count);
bool   paceModbusParse(const uint8_t* rx, size_t n, uint8_t addr, PacePack& p);
uint16_t paceCrc16(const uint8_t* d, size_t n);
