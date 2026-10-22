// Износ батареи. Описание — в battlife.h.
//
// Всё — в loop(): ток берётся снапшотом, счётчики пишутся в NVS раз в
// 15 минут (и после каждой оценки ёмкости) — потерять при выключении
// можно не больше четверти часа.
#include "battlife.h"
#include "state.h"
#include "stats.h"
#include <Preferences.h>
#include <time.h>
#include <algorithm>

static const uint8_t  NEST = 10;                       // последних оценок ёмкости
static const uint32_t SAVE_MS = 15UL * 60 * 1000;
static const float    I_MIN = 0.3f;                    // меньше — шум датчика тока, не ток

struct Est { uint32_t t; float cap; uint8_t s0, _r[3]; };
static_assert(sizeof(Est) == 12, "Est");

static double   s_ahOut = 0, s_ahIn = 0;               // всего, А·ч
static uint32_t s_since = 0;                           // с какого момента считаем
static Est      s_est[NEST];
static uint8_t  s_nEst = 0;
static bool     s_dirty = false;
static uint32_t s_lastMs = 0, s_saveMs = 0;

// Отрезок заряда «до полного» для оценки ёмкости.
static bool     s_seg = false;
static float    s_segS0 = 0;
static double   s_segAh = 0, s_segNeg = 0;
static uint32_t s_segMs = 0;

static void save() {
    Preferences p;
    if (!p.begin("batt", false)) return;
    p.putDouble("out", s_ahOut);
    p.putDouble("in", s_ahIn);
    p.putULong("since", s_since);
    p.putBytes("est", s_est, sizeof(Est) * s_nEst);
    p.end();
    s_dirty = false;
}

void battStart() {
    Preferences p;
    if (p.begin("batt", true)) {
        s_ahOut = p.getDouble("out", 0);
        s_ahIn = p.getDouble("in", 0);
        s_since = p.getULong("since", 0);
        size_t n = p.getBytesLength("est") / sizeof(Est);
        if (n > NEST) n = NEST;
        if (n) p.getBytes("est", s_est, n * sizeof(Est));
        s_nEst = n;
        p.end();
    }
}

float battAhOutTotal() { return (float)s_ahOut; }

static void addEstimate(float cap, float s0) {
    memmove(&s_est[1], &s_est[0], sizeof(Est) * (NEST - 1));
    s_est[0] = { (uint32_t)time(nullptr), cap, (uint8_t)lroundf(s0), { 0, 0, 0 } };
    if (s_nEst < NEST) s_nEst++;
    save();
    Serial.printf("[батарея] оценка ёмкости %.0f А·ч (заряд от %.0f %% до полного)\n", cap, s0);
}

void battLoop() {
    uint32_t ms = millis();
    if (ms - s_lastMs < 1000) return;
    float dt = (ms - s_lastMs) / 1000.0f;
    s_lastMs = ms;
    if (dt > 5) dt = 5;
    BmsState b = stateSnapshot();
    if (!b.fresh) return;
    if (!s_since && statsTimeValid()) { s_since = (uint32_t)time(nullptr); s_dirty = true; }

    float i = b.current;                               // + заряд, − разряд
    if (i < -I_MIN) { s_ahOut += -i * dt / 3600.0; s_dirty = true; }
    if (i > I_MIN)  { s_ahIn  +=  i * dt / 3600.0; s_dirty = true; }

    // Оценка ёмкости: заряд от ≤ 70 % до 100 % без заметного разряда.
    if (b.socKnown && b.capAh > 0) {
        if (!s_seg && i > 1 && b.soc <= 70) {
            s_seg = true; s_segS0 = b.soc; s_segAh = 0; s_segNeg = 0; s_segMs = ms;
        }
        if (s_seg) {
            if (i > 0) s_segAh += i * dt / 3600.0;
            else       s_segNeg += -i * dt / 3600.0;
            if (s_segNeg > 1.0 || ms - s_segMs > 24UL * 3600 * 1000) s_seg = false;   // разряжали посередине — не в счёт
            else if (b.soc >= 99.5f) {
                s_seg = false;
                float span = 100 - s_segS0;
                float cap = span >= 30 ? (float)s_segAh * 100 / span : 0;
                if (cap > b.capAh * 0.4f && cap < b.capAh * 1.3f) addEstimate(cap, s_segS0);   // явный сбой — мимо
            }
        }
    }
    if (s_dirty && ms - s_saveMs > SAVE_MS) { s_saveMs = ms; save(); }
}

// Медиана последних оценок — одна неудачная зарядка картину не портит.
static float estMedian(uint8_t n) {
    if (!s_nEst) return 0;
    if (n > s_nEst) n = s_nEst;
    float v[NEST];
    for (uint8_t k = 0; k < n; k++) v[k] = s_est[k].cap;
    std::sort(v, v + n);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static String dmy(uint32_t t) {
    if (t < 1700000000) return "—";
    time_t loc = (time_t)t + 3 * 3600;
    struct tm lt;
    gmtime_r(&loc, &lt);
    char b[12];
    snprintf(b, sizeof(b), "%02d.%02d.%04d", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900);
    return b;
}

static String num(float v, int dec) { String r = String(v, (unsigned)dec); r.replace('.', ','); return r; }

String battJson() {
    BmsState b = stateSnapshot();
    float cap = b.capAh;
    String j = "{\"cap\":" + String(cap, 1) + ",\"since\":" + String(s_since) +
               ",\"ahOut\":" + String(s_ahOut, 1) + ",\"ahIn\":" + String(s_ahIn, 1) +
               ",\"cycles\":" + String(cap > 0 ? s_ahOut / cap : 0, 2);
    uint32_t d = statsDay();
    StatSum m;
    if (d && statsSum(d - 29, d, m)) {
        j += ",\"m\":{\"ah\":" + String(m.ahOut, 1) + ",\"cycles\":" + String(cap > 0 ? m.ahOut / cap : 0, 2) +
             ",\"socDays\":" + String(m.socDays) + ",\"socMinAvg\":" + String(m.socDays ? (float)m.socMinSum / m.socDays : 0, 0) +
             ",\"deep\":" + String(m.deepDays) + ",\"dataDays\":" + String(m.dataS / 86400.0f, 1) + "}";
    }
    j += ",\"estMed\":" + String(estMedian(5), 0) + ",\"est\":[";
    for (uint8_t k = 0; k < s_nEst; k++) {
        if (k) j += ",";
        j += "{\"t\":" + String(s_est[k].t) + ",\"cap\":" + String(s_est[k].cap, 0) + ",\"s0\":" + String(s_est[k].s0) + "}";
    }
    j += "],\"seg\":" + String(s_seg ? "true" : "false") + ",\"segS0\":" + String(s_segS0, 0) + "}";
    return j;
}

String battText() {
    BmsState b = stateSnapshot();
    float cap = b.capAh;
    String r = "🔋 Износ батареи\n";
    if (cap > 0) r += "ёмкость по BMS: " + num(cap, 0) + " А·ч\n";
    r += "учёт с " + dmy(s_since) + ": разряжено " + num(s_ahOut, 0) + " А·ч";
    if (cap > 0) r += " ≈ " + num(s_ahOut / cap, 1) + " полных циклов";
    r += "\n";
    uint32_t d = statsDay();
    StatSum m;
    if (d && statsSum(d - 29, d, m) && m.dataS) {
        r += "за 30 дней: " + (cap > 0 ? num(m.ahOut / cap, 1) + " цикла, " : String("")) +
             (m.socDays ? "в среднем разряд до " + num((float)m.socMinSum / m.socDays, 0) + " %" : String("заряд не знали")) +
             (m.deepDays ? ", глубоких (до 20 % и ниже): " + String(m.deepDays) : String(", глубоких не было")) + "\n";
    }
    if (s_nEst) {
        float med = estMedian(5);
        r += "оценка ёмкости по заряду до полного: " + num(med, 0) + " А·ч";
        if (cap > 0) r += " (" + num(med / cap * 100, 0) + " % от BMS)";
        r += ", измерений " + String(s_nEst);
    } else {
        r += "оценки ёмкости пока нет — нужна зарядка от 70 % и ниже до полной без разряда посередине";
    }
    return r;
}

String battMonthText() {
    BmsState b = stateSnapshot();
    float cap = b.capAh;
    uint32_t d = statsDay();
    if (!d) return "";
    uint32_t to = d - 1;                               // 1-го числа: вчера — последний день месяца
    time_t t = (time_t)to * 86400;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    uint32_t from = to - (tmv.tm_mday - 1);
    StatSum m;
    if (!statsSum(from, to, m) || !m.dataS) return "";
    String r = "🔋 Батарея за месяц: ";
    r += cap > 0 ? num(m.ahOut / cap, 1) + " полных циклов (всего " + num(s_ahOut / cap, 0) + ")" : num(m.ahOut, 0) + " А·ч разряда";
    if (m.socDays) r += ", в среднем разряд до " + num((float)m.socMinSum / m.socDays, 0) + " %";
    r += m.deepDays ? ", глубоких разрядов: " + String(m.deepDays) : String(", глубоких разрядов не было");
    if (s_nEst && cap > 0) r += "\nоценка ёмкости " + num(estMedian(5), 0) + " А·ч (" + num(estMedian(5) / cap * 100, 0) + " % от BMS)";
    return r;
}
