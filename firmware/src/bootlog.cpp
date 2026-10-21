#include "bootlog.h"
#include "stats.h"
#include <Preferences.h>
#include <time.h>
#include "esp_system.h"
#include "esp_attr.h"

static const uint32_t RTC_MAGIC = 0x424F4F54;       // "BOOT"
static const uint8_t  NBOOT = 10;

// Переживает программную перезагрузку, при пропаже питания — мусор,
// поэтому под магией.
RTC_NOINIT_ATTR static uint32_t r_magic;
RTC_NOINIT_ATTR static uint32_t r_upS;              // сколько проработала прошлая загрузка
RTC_NOINIT_ATTR static char     r_why[40];          // подпись программной перезагрузки

struct BootRec { uint32_t epoch; uint32_t prevUpS; uint8_t reason; char why[23]; };
static_assert(sizeof(BootRec) == 32, "BootRec");

static BootRec  s_list[NBOOT];
static uint8_t  s_n = 0;                            // записей в s_list, новая — [0]
static uint8_t  s_reason = 0;
static String   s_why;
static uint32_t s_prevUpS = 0;
static bool     s_dated = false;

static const char* reasonName(uint8_t r) {
    switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON:   return "включение питания";
    case ESP_RST_EXT:       return "кнопка RST";
    case ESP_RST_SW:        return "перезагрузка программой";
    case ESP_RST_PANIC:     return "сбой программы";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "зависание (сторожевой таймер)";
    case ESP_RST_BROWNOUT:  return "просадка питания";
    case ESP_RST_DEEPSLEEP: return "выход из сна";
    case ESP_RST_USB:       return "по USB";
    default:                return "неизвестно";
    }
}

static bool badReason(uint8_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

static void save() {
    Preferences p;
    if (!p.begin("boots", false)) return;
    p.putBytes("list", s_list, sizeof(BootRec) * s_n);
    p.end();
}

void bootStart() {
    s_reason = (uint8_t)esp_reset_reason();
    bool rtcOk = r_magic == RTC_MAGIC && s_reason != ESP_RST_POWERON && s_reason != ESP_RST_BROWNOUT;
    if (rtcOk) {
        s_prevUpS = r_upS;
        r_why[sizeof(r_why) - 1] = 0;
        if (s_reason == ESP_RST_SW) s_why = r_why;
    }
    r_magic = RTC_MAGIC;
    r_upS = 0;
    r_why[0] = 0;

    Preferences p;
    if (p.begin("boots", true)) {
        size_t n = p.getBytesLength("list") / sizeof(BootRec);
        if (n > NBOOT) n = NBOOT;
        if (n) p.getBytes("list", s_list, n * sizeof(BootRec));
        s_n = n;
        p.end();
    }
    memmove(&s_list[1], &s_list[0], sizeof(BootRec) * (NBOOT - 1));
    memset(&s_list[0], 0, sizeof(BootRec));
    s_list[0].reason = s_reason;
    s_list[0].prevUpS = s_prevUpS;
    strncpy(s_list[0].why, s_why.c_str(), sizeof(s_list[0].why) - 1);
    if (s_n < NBOOT) s_n++;
    save();
    Serial.printf("[boot] причина: %s\n", bootReason().c_str());
}

void bootLoop() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    r_upS = millis() / 1000;
    if (!s_dated && statsTimeValid()) {              // часы появились — проставить дату загрузки
        s_dated = true;
        s_list[0].epoch = (uint32_t)time(nullptr) - millis() / 1000;
        save();
    }
}

void bootMark(const char* why) {
    strncpy(r_why, why, sizeof(r_why) - 1);
    r_why[sizeof(r_why) - 1] = 0;
    r_upS = millis() / 1000;
}

String bootReason() {
    String r = reasonName(s_reason);
    if (s_why.length()) r += ": " + s_why;
    return r;
}

bool bootWasBad() { return badReason(s_reason); }

static String upText(uint32_t s) {
    if (!s) return "";
    char b[32];
    if (s >= 86400) snprintf(b, sizeof(b), "%lu д %lu ч", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
    else if (s >= 3600) snprintf(b, sizeof(b), "%lu ч %lu мин", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else if (s >= 60) snprintf(b, sizeof(b), "%lu мин", (unsigned long)(s / 60));
    else snprintf(b, sizeof(b), "меньше минуты");
    return b;
}

static String when(uint32_t epoch) {
    if (epoch < 1700000000) return "—";
    time_t loc = (time_t)epoch + 3 * 3600;
    struct tm lt;
    gmtime_r(&loc, &lt);
    char b[20];
    snprintf(b, sizeof(b), "%02d.%02d %02d:%02d", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
    return b;
}

static String esc(const String& s) { String o = s; o.replace("\"", "'"); return o; }

String bootJson() {
    String j = "{\"reason\":\"" + esc(bootReason()) + "\",\"bad\":" + (bootWasBad() ? "true" : "false") +
               ",\"prevUpS\":" + String(s_prevUpS) + ",\"list\":[";
    for (uint8_t i = 0; i < s_n; i++) {
        const BootRec& b = s_list[i];
        String r = reasonName(b.reason);
        if (b.why[0]) r += String(": ") + b.why;
        if (i) j += ",";
        j += "{\"t\":" + String(b.epoch) + ",\"r\":\"" + esc(r) + "\",\"bad\":" + (badReason(b.reason) ? "true" : "false") +
             ",\"up\":" + String(b.prevUpS) + "}";
    }
    return j + "]}";
}

String bootText() {
    String r = "перезапуск: " + bootReason();
    if (s_prevUpS) r += ", до этого работал " + upText(s_prevUpS);
    uint8_t bad = 0;
    for (uint8_t i = 0; i < s_n; i++) if (badReason(s_list[i].reason)) bad++;
    if (bad) r += "\n⚠️ сбоев среди последних " + String(s_n) + " перезапусков: " + String(bad);
    r += "\nпоследние перезапуски:";
    for (uint8_t i = 0; i < s_n && i < 5; i++) {
        const BootRec& b = s_list[i];
        r += "\n" + String(badReason(b.reason) ? "⚠️ " : "• ") + when(b.epoch) + " " + reasonName(b.reason);
        if (b.why[0]) r += String(": ") + b.why;
    }
    return r;
}
