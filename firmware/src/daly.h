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
    bool    hasEybond;      // рекламирует сервис донгла инвертора
};

void   dalyStart();                      // поднять задачу BLE на ядре 0
void   dalySetBle(bool on);              // Bluetooth не поднят — поиск и опрос батареи молчат
bool   dalyBle();
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

// Журнал Bluetooth: строка уходит в Serial и в кольцо последних строк, которое
// отдаёт /api/ble/log. Плата висит у инвертора без кабеля, и иначе причину
// «батарея не находится» не увидеть.
void   bleLog(const char* fmt, ...);
String bleLogText();
String dalyD2Report();
void   dalyD2Read(uint16_t start, uint16_t count);   // чтение регистров D2 (функция 03)
String dalyD2ReadReport();

// Пороги защиты BMS из блока настроек протокола D2 (регистры 128–168).
// Разбор сверен 26.10.2026 на DL-41190401535D: пороги батареи ровно в 14 раз
// больше порогов ячейки (3980/4080/3100/3000 мВ ↔ 55,6/57,0/43,4/42,0 В).
struct BmsLimits {
    bool     have = false;
    float    ratedAh = 0;                       // 128, ×0,1 — что записано в BMS
    uint16_t cellOvWarn = 0, cellOvProt = 0;    // 139, 140 — мВ
    uint16_t cellUvWarn = 0, cellUvProt = 0;    // 141, 142
    float    packOvWarn = 0, packOvProt = 0;    // 143, 144 — В
    float    packUvWarn = 0, packUvProt = 0;    // 145, 146
    float    dsgWarnA = 0, dsgProtA = 0;        // 147, 148 — (30000 − x) × 0,1
    float    chgWarnA = 0, chgProtA = 0;        // 149, 150 — (x − 30000) × 0,1
    uint16_t r164 = 0;                          // 50 — похоже, порог разброса, мВ; не сверено
    uint32_t ms = 0;
};
BmsLimits dalyLimits();     // последний ответ протокола D2 — регистры для сверки
