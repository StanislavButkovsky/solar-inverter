// Статистика за год. Описание и раскладка файлов — в stats.h.
//
// Всё здесь крутится в loop(), в одной задаче с веб-сервером: копит
// statsLoop(), читают обработчики HTTP — гонок нет, мьютекс не нужен.
#include "stats.h"
#include "state.h"
#include "rtc.h"
#include "tg.h"
#include <LittleFS.h>
#include <sys/time.h>
#include <time.h>
#include "esp_system.h"

static const int32_t  TZ_SEC   = 3 * 3600;           // как configTime() в net.cpp
static const uint16_t NDAYS    = 366, NHOURS = 168, NOUT = 1000;
// Раз в минуту, а не в пять: плата перезагружается сама, когда не находит
// Wi-Fi, — а без роутера она остаётся как раз при отключении сети, и пятиминутный
// интервал терял почти всё отключение. 96 байт в минуту на 1,5 МБ раздела с
// выравниванием износа — это десятилетия.
static const uint32_t FLUSH_MS = 60UL * 1000;
static const uint32_t OUT_MAGIC = 0x4F555431;          // "OUT1"

static const char* F_DAYS  = "/stats/days.bin";
static const char* F_HOURS = "/stats/hours.bin";
static const char* F_OUT   = "/stats/outages.bin";

// day = сутки от 1970 года по местному времени; 0 — пустая ячейка.
// Поля стоят по убыванию размера: выравнивание естественное, упаковка не
// нужна, и в файле те же 64 байта, что в памяти.
struct DayRec {
    uint32_t day;
    float    gridWh, loadWh, battWh, pvWh, chgWh;
    uint32_t outageS, dataS;
    float    gridVmin, gridVmax, battVmin, battVmax;   // 0 — значений не было
    uint16_t outages;
    int16_t  tempMax;
    uint8_t  loadMax, _r0;
    uint8_t  _r[10];
};
static_assert(sizeof(DayRec) == 64, "DayRec");

struct __attribute__((packed)) HourRec {
    uint32_t hour;                                    // часы от 1970 года, местные
    float    gridWh, loadWh, battWh, pvWh, chgWh;
    uint16_t outageS, dataS;
    uint8_t  _r[4];
};
static_assert(sizeof(HourRec) == 32, "HourRec");

struct __attribute__((packed)) OutRec { uint32_t start, end; uint8_t open, _r[3]; };
struct __attribute__((packed)) OutHdr { uint32_t magic, next, count; };

static bool     s_fs = false;
static DayRec   s_day,  s_pend;                       // s_pend — пока нет времени
static HourRec  s_hour, s_pendH;
static bool     s_bound = false;                      // s_day привязан к календарю
static uint32_t s_lastSample = 0, s_lastFlush = 0;
static float    s_carry = 0;                          // доли секунды для dataS/outageS

// Последние показания счётчиков инвертора: приращение между ними и идёт в
// статистику. Скачок больше 5 кВт·ч за секунды — сброс или сбой счётчика,
// такое не засчитываем.
static double   s_cPv = 0, s_cGrid = 0, s_cLoad = 0;

static float viaCounter(double now, double& last, float watts, float k) {
    if (now <= 0) return watts * k;      // счётчика нет — интегрируем мощность
    float wh = (last > 0 && now >= last && now - last < 5) ? (float)((now - last) * 1000.0) : 0;
    last = now;
    return wh;
}

// Отключение засчитывается, только если сети нет дольше OUT_CONFIRM_MS
// подряд, — и тогда задним числом, с первого показания. 20.10.2026 сбойный
// разбор (ответ батареи 49,1 В принимался за напряжение сети) дал десятки
// «отключений», которых не было; одно ложное показание больше их не делает.
static const uint32_t OUT_CONFIRM_MS = 10000;
static uint32_t s_lossSince = 0;         // millis() первого показания «сети нет»

static bool     s_inOut = false;
static uint32_t s_outStartMs = 0;
static int32_t  s_outSlot = -1;                       // ячейка текущего отключения в файле

// ------------------------------------------------------------------ время

bool statsTimeValid() { return time(nullptr) > 1700000000; }

void statsTodayOutages(uint32_t& seconds, uint16_t& count) {
    const DayRec& d = s_bound ? s_day : s_pend;
    seconds = d.outageS;
    count = d.outages;
}

void statsToday(float& pvWh, float& gridWh, float& loadWh) {
    const DayRec& d = s_bound ? s_day : s_pend;
    pvWh = d.pvWh; gridWh = d.gridWh; loadWh = d.loadWh;
}

static uint32_t localNow() { return (uint32_t)(time(nullptr) + TZ_SEC); }
static uint32_t today()    { return localNow() / 86400; }
static uint32_t thisHour() { return localNow() / 3600; }

void statsSetTime(uint32_t epoch) {
    if (statsTimeValid() || epoch < 1700000000) return;
    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, nullptr);
    rtcTimeSetExternally();               // и в модуль часов, если он есть
    Serial.println("[стат] время взято из браузера");
}

// ------------------------------------------------------------------ файлы

static void ensureFile(const char* path, size_t size) {
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        size_t have = f.size();
        f.close();
        if (have == size) return;
    }
    File f = LittleFS.open(path, "w");
    uint8_t zero[64] = {0};
    for (size_t done = 0; done < size; done += sizeof(zero))
        f.write(zero, size - done < sizeof(zero) ? size - done : sizeof(zero));
    f.close();
}

static void readAt(const char* path, size_t off, void* buf, size_t n) {
    memset(buf, 0, n);
    File f = LittleFS.open(path, "r");
    if (!f) return;
    f.seek(off);
    f.read((uint8_t*)buf, n);
    f.close();
}

static void writeAt(const char* path, size_t off, const void* buf, size_t n) {
    File f = LittleFS.open(path, "r+");
    if (!f) return;
    f.seek(off);
    f.write((const uint8_t*)buf, n);
    f.close();
}

static void readDay(uint32_t d, DayRec& r)    { readAt(F_DAYS, (d % NDAYS) * sizeof(DayRec), &r, sizeof(r)); }
static void writeDay(const DayRec& r)         { writeAt(F_DAYS, (r.day % NDAYS) * sizeof(DayRec), &r, sizeof(r)); }
static void readHour(uint32_t h, HourRec& r)  { readAt(F_HOURS, (h % NHOURS) * sizeof(HourRec), &r, sizeof(r)); }
static void writeHour(const HourRec& r)       { writeAt(F_HOURS, (r.hour % NHOURS) * sizeof(HourRec), &r, sizeof(r)); }

static OutHdr outHdr() {
    OutHdr h;
    readAt(F_OUT, 0, &h, sizeof(h));
    if (h.magic != OUT_MAGIC) h = { OUT_MAGIC, 0, 0 };
    return h;
}

static size_t outOff(uint32_t slot) { return sizeof(OutHdr) + slot * sizeof(OutRec); }

static int32_t outAppend(const OutRec& r) {
    OutHdr h = outHdr();
    uint32_t slot = h.next;
    writeAt(F_OUT, outOff(slot), &r, sizeof(r));
    h.next = (h.next + 1) % NOUT;
    if (h.count < NOUT) h.count++;
    writeAt(F_OUT, 0, &h, sizeof(h));
    return (int32_t)slot;
}

// ------------------------------------------------------------------ накопление

static void minmax(float& lo, float& hi, float v) {
    if (v <= 0) return;
    if (lo == 0 || v < lo) lo = v;
    if (v > hi) hi = v;
}

static void mergeDay(DayRec& a, const DayRec& b) {
    a.gridWh += b.gridWh; a.loadWh += b.loadWh; a.battWh += b.battWh;
    a.pvWh += b.pvWh; a.chgWh += b.chgWh;
    a.outageS += b.outageS; a.dataS += b.dataS; a.outages += b.outages;
    if (b.tempMax > a.tempMax) a.tempMax = b.tempMax;
    if (b.loadMax > a.loadMax) a.loadMax = b.loadMax;
    minmax(a.gridVmin, a.gridVmax, b.gridVmin); minmax(a.gridVmin, a.gridVmax, b.gridVmax);
    minmax(a.battVmin, a.battVmax, b.battVmin); minmax(a.battVmin, a.battVmax, b.battVmax);
}

static void mergeHour(HourRec& a, const HourRec& b) {
    a.gridWh += b.gridWh; a.loadWh += b.loadWh; a.battWh += b.battWh;
    a.pvWh += b.pvWh; a.chgWh += b.chgWh;
    a.outageS += b.outageS; a.dataS += b.dataS;
}

static void flush() {
    s_lastFlush = millis();
    if (!s_fs || !s_bound) return;
    writeDay(s_day);
    writeHour(s_hour);
    // Текущее отключение: конец «пока что» — чтобы после пропажи питания
    // в журнале остался хотя бы его нижний предел.
    if (s_inOut) {
        uint32_t now = (uint32_t)time(nullptr);
        if (s_outSlot < 0) {
            OutRec r = { now - (millis() - s_outStartMs) / 1000, now, 1, {0} };
            s_outSlot = outAppend(r);
        } else {
            OutRec r;
            readAt(F_OUT, outOff(s_outSlot), &r, sizeof(r));
            r.end = now;
            writeAt(F_OUT, outOff(s_outSlot), &r, sizeof(r));
        }
    }
}

// Время появилось: подхватить сегодняшний день из файла и влить в него
// то, что накопилось без календаря.
static void bindTime() {
    uint32_t d = today(), h = thisHour();
    readDay(d, s_day);
    if (s_day.day != d) { memset(&s_day, 0, sizeof(s_day)); s_day.day = d; }
    readHour(h, s_hour);
    if (s_hour.hour != h) { memset(&s_hour, 0, sizeof(s_hour)); s_hour.hour = h; }
    mergeDay(s_day, s_pend);
    mergeHour(s_hour, s_pendH);
    memset(&s_pend, 0, sizeof(s_pend));
    memset(&s_pendH, 0, sizeof(s_pendH));
    s_bound = true;
    flush();
}

static void rollover() {
    uint32_t d = today(), h = thisHour();
    if (h != s_hour.hour) {
        writeHour(s_hour);
        readHour(h, s_hour);
        if (s_hour.hour != h) { memset(&s_hour, 0, sizeof(s_hour)); s_hour.hour = h; }
    }
    if (d != s_day.day) {
        writeDay(s_day);
        readDay(d, s_day);
        if (s_day.day != d) { memset(&s_day, 0, sizeof(s_day)); s_day.day = d; }
        // Отключение, идущее через полночь, считается и в новом дне.
        if (s_inOut) s_day.outages++;
    }
}

static void outageStart(DayRec& D, HourRec& H) {
    s_inOut = true;
    s_outStartMs = s_lossSince;          // задним числом — с первого показания
    s_outSlot = -1;
    D.outages++;
    uint32_t back = (millis() - s_lossSince) / 1000;
    D.outageS += back;
    H.outageS += back;
    tgOutage(true, back);
    if (s_bound) {
        uint32_t now = (uint32_t)time(nullptr);
        OutRec r = { now - back, now, 1, {0} };
        s_outSlot = outAppend(r);
    }
}

static void outageEnd() {
    s_inOut = false;
    tgOutage(false, (millis() - s_outStartMs) / 1000);
    if (!s_bound) return;                 // без времени записать нечего, счётчики в дне есть
    uint32_t now = (uint32_t)time(nullptr);
    OutRec r;
    if (s_outSlot >= 0) {
        readAt(F_OUT, outOff(s_outSlot), &r, sizeof(r));
        r.end = now;
        r.open = 0;
        writeAt(F_OUT, outOff(s_outSlot), &r, sizeof(r));
    } else {
        r = { now - (millis() - s_outStartMs) / 1000, now, 0, {0} };
        outAppend(r);
    }
    s_outSlot = -1;
}

void statsLoop() {
    uint32_t ms = millis();
    if (ms - s_lastSample < 1000) return;
    float dt = (ms - s_lastSample) / 1000.0f;
    s_lastSample = ms;
    if (dt > 5) dt = 5;                   // после долгой паузы не выдумываем данные

    if (statsTimeValid()) {
        if (!s_bound) bindTime();
        else rollover();
    }
    DayRec&  D = s_bound ? s_day  : s_pend;
    HourRec& H = s_bound ? s_hour : s_pendH;

    InvState v = invSnapshot();
    BmsState b = stateSnapshot();
    if (v.linked && v.fresh) {
        s_carry += dt;
        uint32_t whole = (uint32_t)s_carry;
        s_carry -= whole;
        D.dataS += whole;
        H.dataS += whole;

        float k = dt / 3600.0f;
        float batt = v.mode == INVM_BATTERY ? v.outW : 0;
        float grid = viaCounter(v.gridKwhTotal, s_cGrid, v.gridW, k);
        float load = viaCounter(v.loadKwhTotal, s_cLoad, v.outW,  k);
        float pv   = viaCounter(v.pvKwhTotal,   s_cPv,   v.pvW,   k);
        D.gridWh += grid;         H.gridWh += grid;
        D.loadWh += load;         H.loadWh += load;
        D.battWh += batt    * k;  H.battWh += batt    * k;
        D.pvWh   += pv;           H.pvWh   += pv;
        D.chgWh  += v.chgW  * k;  H.chgWh  += v.chgW  * k;

        if (v.gridOn) minmax(D.gridVmin, D.gridVmax, v.gridV);
        if (b.fresh)  minmax(D.battVmin, D.battVmax, b.voltage);
        if (v.tempC > D.tempMax)   D.tempMax = v.tempC;
        if (v.loadPct > D.loadMax) D.loadMax = v.loadPct;

        // Отключение считаем только по живым данным: пока связи с
        // инвертором нет, неизвестно, есть ли сеть, и счётчик стоит.
        if (!v.gridOn) {
            if (!s_lossSince) s_lossSince = ms;
            if (s_inOut) {
                D.outageS += whole;
                H.outageS += whole;
            } else if (ms - s_lossSince >= OUT_CONFIRM_MS) {
                outageStart(D, H);
            }
        } else {
            s_lossSince = 0;
            if (s_inOut) outageEnd();
        }
    }

    if (ms - s_lastFlush > FLUSH_MS) flush();
}

void statsStart() {
    s_fs = LittleFS.begin(true);          // раздел spiffs; пустой — отформатирует
    if (!s_fs) { Serial.println("[стат] файловая система не поднялась, статистика только в памяти"); return; }
    LittleFS.mkdir("/stats");
    ensureFile(F_DAYS,  NDAYS  * sizeof(DayRec));
    ensureFile(F_HOURS, NHOURS * sizeof(HourRec));
    ensureFile(F_OUT,   sizeof(OutHdr) + NOUT * sizeof(OutRec));

    // Отключение, оборванное пропажей питания платы, закрываем тем концом,
    // что успели записать: это нижний предел его длительности.
    OutHdr h = outHdr();
    if (h.count) {
        uint32_t last = (h.next + NOUT - 1) % NOUT;
        OutRec r;
        readAt(F_OUT, outOff(last), &r, sizeof(r));
        if (r.open) { r.open = 0; writeAt(F_OUT, outOff(last), &r, sizeof(r)); }
    }
    s_lastSample = s_lastFlush = millis();
    // Любая штатная перезагрузка — по команде, после смены сети, по потере
    // Wi-Fi — сначала дописывает накопленное. Падение сюда не заходит.
    esp_register_shutdown_handler([]() { flush(); });
    Serial.printf("[стат] файлы на месте, занято %u из %u КБ\n",
                  (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
}

// ------------------------------------------------------------------ выдача

static void put(String& j, const char* k, float v, int dec) { j += ",\""; j += k; j += "\":"; j += String(v, dec); }
static void put(String& j, const char* k, uint32_t v)       { j += ",\""; j += k; j += "\":"; j += String(v); }

// Столбик: подпись t (час, сутки от 1970 или месяц как год*12+месяц) и суммы.
static void bar(String& j, uint32_t t, const DayRec& r, bool first) {
    if (!first) j += ",";
    j += "{\"t\":"; j += String(t);
    put(j, "g", r.gridWh, 0); put(j, "l", r.loadWh, 0); put(j, "b", r.battWh, 0);
    put(j, "p", r.pvWh, 0);   put(j, "c", r.chgWh, 0);
    put(j, "o", r.outageS);   put(j, "n", (uint32_t)r.outages); put(j, "d", r.dataS);
    j += "}";
}

static DayRec fromHour(const HourRec& h) {
    DayRec r;
    memset(&r, 0, sizeof(r));
    r.gridWh = h.gridWh; r.loadWh = h.loadWh; r.battWh = h.battWh;
    r.pvWh = h.pvWh; r.chgWh = h.chgWh; r.outageS = h.outageS; r.dataS = h.dataS;
    return r;
}

static void dayAt(uint32_t d, DayRec& r) {
    if (d == s_day.day) { r = s_day; return; }
    readDay(d, r);
    if (r.day != d) { memset(&r, 0, sizeof(r)); r.day = d; }
}

static void totals(String& j, const DayRec& t) {
    j += ",\"tot\":{\"x\":0";
    put(j, "g", t.gridWh, 0); put(j, "l", t.loadWh, 0); put(j, "b", t.battWh, 0);
    put(j, "p", t.pvWh, 0);   put(j, "c", t.chgWh, 0);
    put(j, "o", t.outageS);   put(j, "n", (uint32_t)t.outages); put(j, "d", t.dataS);
    put(j, "gvmin", t.gridVmin, 1); put(j, "gvmax", t.gridVmax, 1);
    put(j, "bvmin", t.battVmin, 1); put(j, "bvmax", t.battVmax, 1);
    put(j, "tmax", (uint32_t)(t.tempMax > 0 ? t.tempMax : 0));
    put(j, "lmax", (uint32_t)t.loadMax);
    j += "}";
}

String statsJson(const String& range) {
    String j;
    j.reserve(3000);
    if (!s_bound) {
        j = "{\"valid\":false";
        totals(j, s_pend);
        return j + "}";
    }
    uint32_t d0 = s_day.day;
    DayRec tot;
    memset(&tot, 0, sizeof(tot));
    j = "{\"valid\":true,\"range\":\"" + range + "\",\"today\":" + String(d0) + ",\"bars\":[";

    if (range == "day") {
        for (uint32_t i = 0; i < 24; i++) {
            uint32_t h = d0 * 24 + i;
            HourRec r;
            if (h == s_hour.hour) r = s_hour;
            else { readHour(h, r); if (r.hour != h) memset(&r, 0, sizeof(r)); }
            bar(j, i, fromHour(r), i == 0);
        }
        tot = s_day;
    } else if (range == "week" || range == "month") {
        uint32_t from = d0 - 6;
        if (range == "month") {
            time_t t = (time_t)d0 * 86400;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            from = d0 - (tmv.tm_mday - 1);
        }
        for (uint32_t d = from; d <= d0; d++) {
            DayRec r;
            dayAt(d, r);
            bar(j, d, r, d == from);
            mergeDay(tot, r);
        }
    } else {                                           // year: 12 месяцев, текущий последним
        time_t t = (time_t)d0 * 86400;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        int32_t mNow = (tmv.tm_year + 1900) * 12 + tmv.tm_mon;
        DayRec months[12];
        memset(months, 0, sizeof(months));
        // Все 366 ячеек одним проходом по файлу; сегодняшний день — из памяти,
        // он новее записанного.
        File f = LittleFS.open(F_DAYS, "r");
        for (uint32_t slot = 0; slot < NDAYS; slot++) {
            DayRec r;
            memset(&r, 0, sizeof(r));
            if (f) f.read((uint8_t*)&r, sizeof(r));
            if (slot == d0 % NDAYS) r = s_day;
            if (!r.day || r.day > d0 || d0 - r.day >= NDAYS) continue;
            time_t td = (time_t)r.day * 86400;
            gmtime_r(&td, &tmv);
            int32_t back = mNow - ((tmv.tm_year + 1900) * 12 + tmv.tm_mon);
            if (back < 0 || back > 11) continue;
            mergeDay(months[11 - back], r);
        }
        if (f) f.close();
        for (int i = 0; i < 12; i++) {
            bar(j, (uint32_t)(mNow - 11 + i), months[i], i == 0);
            mergeDay(tot, months[i]);
        }
    }
    j += "]";
    totals(j, tot);
    j += ",\"inOutage\":"; j += s_inOut ? "true" : "false";
    return j + "}";
}

void statsClear() {
    memset(&s_day, 0, sizeof(s_day));   s_day.day = s_bound ? today() : 0;
    memset(&s_hour, 0, sizeof(s_hour)); s_hour.hour = s_bound ? thisHour() : 0;
    memset(&s_pend, 0, sizeof(s_pend));
    memset(&s_pendH, 0, sizeof(s_pendH));
    s_inOut = false; s_lossSince = 0; s_outSlot = -1;
    s_cPv = s_cGrid = s_cLoad = 0;
    if (!s_fs) return;
    LittleFS.remove(F_DAYS); LittleFS.remove(F_HOURS); LittleFS.remove(F_OUT);
    ensureFile(F_DAYS,  NDAYS  * sizeof(DayRec));
    ensureFile(F_HOURS, NHOURS * sizeof(HourRec));
    ensureFile(F_OUT,   sizeof(OutHdr) + NOUT * sizeof(OutRec));
    Serial.println("[стат] статистика стёрта");
}

String outagesJson(uint16_t n) {
    String j = "[";
    if (s_fs) {
        OutHdr h = outHdr();
        uint32_t cnt = h.count < n ? h.count : n;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t slot = (h.next + NOUT - 1 - i) % NOUT;
            OutRec r;
            readAt(F_OUT, outOff(slot), &r, sizeof(r));
            if (i) j += ",";
            j += "{\"s\":" + String(r.start) + ",\"e\":" + String(r.end) +
                 ",\"open\":" + String(r.open ? "true" : "false") + "}";
        }
    }
    return j + "]";
}
