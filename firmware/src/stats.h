// Статистика за год: энергии, отключения сети, крайние значения.
//
// Раз в секунду берёт снапшот инвертора и батареи и копит: из сети, нагрузку,
// от батареи, солнце, заряд — интегралом мощности. Счётчики кВт·ч инвертора
// не берутся: у HS35 они обновляются порциями раз в час (см. stats.cpp).
// Плюс минуты без сети, число отключений, мин./макс.
// напряжений, максимумы температуры и загрузки. И отдельно — сколько секунд
// данные вообще были: без этого «0 кВт·ч» не отличить от «платы не было».
//
// Хранится в LittleFS (раздел spiffs), сбрасывается раз в минуту и перед
// каждой штатной перезагрузкой:
//   /stats/days.bin     366 дней кольцом, по 64 байта   — 23 КБ
//   /stats/hours.bin    168 часов кольцом, по 32 байта  — 5 КБ
//   /stats/outages.bin  1000 отключений кольцом         — 12 КБ
//   /stats/power.bin    288 средних за 5 минут кольцом  — 4,5 КБ (график за сутки)
// Месяцы и год по месяцам собираются из дней при запросе.
//
// Сутки и часы — по местному времени UTC+3, как у NTP в net.cpp. Пока
// времени нет (ни NTP, ни браузера), накопленное держится в памяти и уходит
// в сегодняшний день, когда время появится.
#pragma once
#include <Arduino.h>

void   statsStart();
void   statsLoop();                       // из loop(): сам решает, когда копить и писать
String statsJson(const String& range);    // day | week | month | year
String outagesJson(uint16_t n);           // последние отключения, новые сверху
void   powerWrite(void (*emit)(const String&));   // мощности за сутки по 5 минут — по частям
bool   statsTimeValid();
void   statsClear();
void   statsToday(float& pvWh, float& gridWh, float& loadWh);   // накоплено за сегодня
void   statsTodayOutages(uint32_t& seconds, uint16_t& count);
void   statsSetPvForecast(float wh);
// Сколько тратит сам инвертор — считается только при работе от батареи
// (см. SelfRec в stats.cpp). Сейчас, Вт: NAN — не от батареи или нет данных.
float  statsSelfW();
void   statsSelfToday(float& wh, uint32_t& seconds);   // за сегодня: энергия и секунды счёта

// Суммы за дни fromDay..toDay включительно (местные сутки от 1970) — для
// сводок за неделю и месяц. Только из loop(): читает файл дней.
struct StatSum {
    float pvWh, gridWh, loadWh, battWh, pvFcWh, ahOut;
    float selfWh; uint32_t selfS;         // сам инвертор: энергия и секунды работы от батареи
    uint32_t outageS, dataS, socMinSum;   // socMinSum — сумма дневных минимумов заряда, %
    uint16_t outages, socDays, deepDays;  // дней, когда заряд знали; из них разряжались до 20 % и ниже
};
bool     statsSum(uint32_t fromDay, uint32_t toDay, StatSum& out);   // false — времени нет
uint32_t statsDay();                      // сегодня, местные сутки от 1970; 0 — времени нет      // прогноз выработки на сегодня — из weather.cpp
// Вся история по дням CSV-таблицей, по частям: для выгрузки и бэкапа в бот.
void   statsCsv(void (*emit)(const String&, void*), void* ctx);
float  statsBattAvgA();                   // средний ток батареи за ~3 мин, А: + заряд, − разряд

// Прогноз: сколько секунд до порога lowPct при разряде или до 100 % при
// заряде; 0 — посчитать нельзя (нет ёмкости, процента или ток мал).
uint32_t battEtaS(float socPct, float capAh, float avgA, uint8_t lowPct);                      // стереть всё накопленное — необратимо
void   statsSetTime(uint32_t epoch);      // время из браузера, только если своего нет
