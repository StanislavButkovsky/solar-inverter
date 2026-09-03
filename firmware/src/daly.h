// Транспорт и протокол DALY поверх BLE.
//
// Донгл DALY — это мост BLE ↔ UART, отдающий тот же нативный протокол, что провод:
// кадры фиксированной длины 13 байт.
//
//   A5 | адрес | DataID | 08 | D0..D7 | CS
//   CS = младший байт суммы первых 12 байт
//
// Адрес отправителя для блютуз-приложения — 0x80 (не 0x40, тот у ПК по UART).
// Ответ приходит уведомлением с адресом 0x01 (сама BMS).
//
//   сервис          FFF0
//   FFF2 (write)    ESP32 → BMS
//   FFF1 (notify)   BMS → ESP32
#pragma once
#include <Arduino.h>
#include <vector>

static const uint8_t DALY_FRAME_LEN = 13;
static const uint8_t DALY_ADDR_HOST = 0x80;   // блютуз-приложение
static const uint8_t DALY_ADDR_BMS  = 0x01;

struct BleFound {
    String  mac;
    String  name;
    int     rssi;
    bool    hasDalyService;
};

void   dalyStart();                      // поднять задачу BLE на ядре 0
void   dalyReleaseFor(uint32_t minutes); // отпустить батарею официальному приложению
bool   dalyIsReleased();
uint32_t dalyReleaseLeftS();
std::vector<BleFound> dalyScan(uint8_t seconds);  // блокирующее сканирование, для CLI

// Асинхронный поиск для веба: запрос ставит флаг, сканирует задача BLE.
// Нужен, чтобы искать батарею с телефона, ходя с модулем по дому.
void   dalyRequestScan();
bool   dalyScanBusy();
std::vector<BleFound> dalyScanResults();
void   dalyBind(const String& mac);   // привязать батарею и переподключиться

// разбор одного валидного кадра — вынесен из транспорта, чтобы переиспользоваться
// один в один для CAN и UART, когда до них дойдёт очередь
void   dalyParseFrame(const uint8_t* f);
