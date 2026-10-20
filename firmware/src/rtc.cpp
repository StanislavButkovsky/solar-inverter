// DS3231: регистры 0x00–0x06 — секунды, минуты, часы, день недели, число,
// месяц (бит 7 — век), год, всё в двоично-десятичном виде. Регистр 0x0F,
// бит 7 (OSF) — генератор останавливался: время в модуле не верно, пока
// его не перезапишут. Своя реализация на Wire — библиотека ради семи
// регистров не нужна.
#include "rtc.h"
#include <Wire.h>
#include <sys/time.h>
#include <time.h>
#include "esp_sntp.h"

static const uint8_t ADDR = 0x68;
static const time_t  VALID_FROM = 1700000000;           // 2023 год: раньше — не время
static const uint32_t RESYNC_MS = 24UL * 3600 * 1000;   // NTP → модуль раз в сутки

static bool              s_present = false;
static bool              s_fromRtc = false;    // системное время взято из модуля
static volatile bool     s_writeReq = false;   // пришло время снаружи — записать
static uint32_t          s_lastWrite = 0;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static bool readRegs(uint8_t reg, uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(ADDR, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}

static bool writeRegs(uint8_t reg, const uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    for (uint8_t i = 0; i < n; i++) Wire.write(buf[i]);
    return Wire.endTransmission() == 0;
}

// Время из модуля, UTC. 0 — модуля нет, генератор останавливался или мусор.
static time_t rtcRead() {
    uint8_t st;
    if (!readRegs(0x0F, &st, 1) || (st & 0x80)) return 0;
    uint8_t r[7];
    if (!readRegs(0x00, r, 7)) return 0;
    struct tm t = {};
    t.tm_sec  = bcd2bin(r[0] & 0x7F);
    t.tm_min  = bcd2bin(r[1] & 0x7F);
    t.tm_hour = bcd2bin(r[2] & 0x3F);                  // 24-часовой режим
    t.tm_mday = bcd2bin(r[4] & 0x3F);
    t.tm_mon  = bcd2bin(r[5] & 0x1F) - 1;
    t.tm_year = bcd2bin(r[6]) + 100 + ((r[5] & 0x80) ? 100 : 0);
    // mktime работает в местном поясе; на модуле UTC — переводим без него.
    time_t days = 0;
    for (int y = 1970; y < t.tm_year + 1900; y++)
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int y = t.tm_year + 1900;
    for (int m = 0; m < t.tm_mon && m < 12; m++)
        days += mdays[m] + (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0);
    days += t.tm_mday - 1;
    time_t v = days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    return v >= VALID_FROM ? v : 0;
}

static bool rtcWrite(time_t now) {
    struct tm t;
    gmtime_r(&now, &t);
    int year = t.tm_year + 1900;
    uint8_t r[7] = {
        bin2bcd(t.tm_sec), bin2bcd(t.tm_min), bin2bcd(t.tm_hour),
        (uint8_t)(t.tm_wday ? t.tm_wday : 7), bin2bcd(t.tm_mday),
        (uint8_t)(bin2bcd(t.tm_mon + 1) | (year >= 2100 ? 0x80 : 0)),
        bin2bcd(year % 100),
    };
    if (!writeRegs(0x00, r, 7)) return false;
    uint8_t st;
    if (readRegs(0x0F, &st, 1)) { st &= ~0x80; writeRegs(0x0F, &st, 1); }   // время снова верно
    return true;
}

// NTP синхронизировался — время свежее, его стоит записать в модуль.
static void onNtp(struct timeval*) { s_writeReq = true; }

void rtcStart() {
    Wire.begin(RTC_SDA, RTC_SCL, 100000);
    Wire.beginTransmission(ADDR);
    s_present = Wire.endTransmission() == 0;
    sntp_set_time_sync_notification_cb(onNtp);
    if (!s_present) {
        Serial.printf("[rtc] модуля DS3231 нет (SDA %d, SCL %d) — время только по NTP и с телефона\n", RTC_SDA, RTC_SCL);
        return;
    }
    time_t t = rtcRead();
    if (!t) {
        Serial.println("[rtc] DS3231 есть, но время в нём неверно (села батарейка?) — запишу, как только появится");
        return;
    }
    if (time(nullptr) < VALID_FROM) {
        struct timeval tv = { t, 0 };
        settimeofday(&tv, nullptr);
        s_fromRtc = true;
    }
    time_t loc = t + 3 * 3600;
    struct tm lt;
    gmtime_r(&loc, &lt);
    Serial.printf("[rtc] время из DS3231: %02d.%02d.%04d %02d:%02d (UTC+3)\n",
                  lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900, lt.tm_hour, lt.tm_min);
}

void rtcTimeSetExternally() { s_writeReq = true; s_fromRtc = false; }

void rtcLoop() {
    if (!s_present || time(nullptr) < VALID_FROM) return;
    bool resync = s_lastWrite && !s_fromRtc && millis() - s_lastWrite > RESYNC_MS;
    if (!s_writeReq && !resync) return;
    s_writeReq = false;
    s_fromRtc = false;
    if (rtcWrite(time(nullptr))) {
        s_lastWrite = millis();
        if (!s_lastWrite) s_lastWrite = 1;
        Serial.println("[rtc] время записано в DS3231");
    }
}

String rtcStatus() {
    if (!s_present) return "модуля DS3231 нет";
    time_t t = rtcRead();
    if (!t) return "DS3231 есть, время в нём неверно";
    time_t loc = t + 3 * 3600;
    struct tm lt;
    gmtime_r(&loc, &lt);
    char b[64];
    snprintf(b, sizeof(b), "DS3231: %02d.%02d.%04d %02d:%02d:%02d", lt.tm_mday, lt.tm_mon + 1,
             lt.tm_year + 1900, lt.tm_hour, lt.tm_min, lt.tm_sec);
    return b;
}
