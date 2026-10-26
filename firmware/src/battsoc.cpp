#include "battsoc.h"
#include "cfg.h"
#include "state.h"
#include "events.h"
#include "json.h"
#include <Preferences.h>
#if !defined(NO_BLE)
#include "daly.h"
#endif

// Точки «полная» и «пустая» — для измерения ёмкости между ними.
enum : uint8_t { ANC_NONE = 0, ANC_FULL = 1, ANC_EMPTY = 2 };

static float    s_ah = -1;          // остаток, А·ч; −1 — ещё не начат
static double   s_cum = 0;          // интеграл тока с загрузки, А·ч (+ заряд)
static uint8_t  s_anchor = ANC_NONE;
static double   s_anchorCum = 0;    // s_cum в момент последней точки
static float    s_meas[5];          // измеренные ёмкости, А·ч, новые в конце
static uint8_t  s_measN = 0;
static uint32_t s_lastMs = 0, s_saveMs = 0;
static uint32_t s_fullSince = 0, s_emptySince = 0, s_restSince = 0;
static bool     s_isFull = false, s_isEmpty = false;

bool battSocOn() { return cfg().bCapAh > 1; }

static void save() {
    Preferences p;
    if (!p.begin("bsoc", false)) return;
    p.putFloat("ah", s_ah);
    p.putUChar("anc", s_anchor);
    p.putFloat("since", (float)(s_cum - s_anchorCum));
    p.putBytes("meas", s_meas, sizeof(float) * s_measN);
    p.end();
    s_saveMs = millis();
}

void battSocStart() {
    Preferences p;
    if (!p.begin("bsoc", true)) return;
    s_ah = p.getFloat("ah", -1);
    s_anchor = p.getUChar("anc", ANC_NONE);
    s_anchorCum = -p.getFloat("since", 0);        // s_cum начинается с нуля
    size_t n = p.getBytesLength("meas") / sizeof(float);
    if (n > 5) n = 5;
    if (n) p.getBytes("meas", s_meas, n * sizeof(float));
    s_measN = n;
    p.end();
}

// Напряжение покоя → %, ячейка Leaf (LMO/NCO), отмасштабировано так, что
// 3,40 В — 0 %, 4,05 В — 100 %; при других точках ось напряжений растягивается.
// Таблица приблизительная — поэтому покой поправляет счёт, только когда тот
// ушёл больше чем на 10 %.
static float ocvSoc(float mv) {
    static const uint16_t V[] = {3400, 3500, 3580, 3650, 3720, 3770, 3820, 3870, 3920, 3970, 4010, 4050};
    static const uint8_t  P[] = {   0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100};
    const uint8_t N = sizeof(V) / sizeof(V[0]);
    float e = cfg().bEmptyMv, f = cfg().bFullMv;
    float x = 3400 + (mv - e) * (4050 - 3400) / (f - e);   // в оси таблицы
    if (x <= V[0]) return 0;
    if (x >= V[N - 1]) return 100;
    for (uint8_t i = 1; i < N; i++)
        if (x <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (x - V[i - 1]) / (V[i] - V[i - 1]);
    return 100;
}

static void addMeas(float ah, const char* how) {
    float cap = cfg().bCapAh;
    if (ah < cap * 0.2f || ah > cap * 3) {
        Serial.printf("[заряд] измерение %.1f А·ч (%s) не похоже на правду — не беру\n", ah, how);
        return;
    }
    if (s_measN == 5) { memmove(s_meas, s_meas + 1, 4 * sizeof(float)); s_measN = 4; }
    s_meas[s_measN++] = ah;
    char d[64];
    snprintf(d, sizeof(d), "измерено %.1f А·ч (%s)", ah, how);
    evAdd(EV_BATT_OK, d);
}

// ---------------------------------------------------------- предупреждения

struct Warn { const char* key; bool on; uint32_t since; char text[72]; };
static Warn s_w[7] = {{"ovp"}, {"ovf"}, {"uvp"}, {"uv0"}, {"spr"}, {"cold"}, {"hot"}};

// Условие держится on_s секунд — предупреждение; ушло на off_s — снято.
static void warnSet(uint8_t i, bool cond, bool alarm, const char* text, uint16_t on_s, uint16_t off_s) {
    Warn& w = s_w[i];
    uint32_t now = millis();
    if (cond != w.on) {
        if (!w.since) w.since = now;
        if (now - w.since >= (uint32_t)(cond ? on_s : off_s) * 1000UL) {
            w.on = cond;
            w.since = 0;
            if (cond) { strlcpy(w.text, text, sizeof(w.text)); evAdd(alarm ? EV_BATT_ALARM : EV_BATT_WARN, w.text); }
            else      { evAdd(EV_BATT_OK, w.text); }
        }
    } else {
        w.since = 0;
        if (cond) strlcpy(w.text, text, sizeof(w.text));   // цифры свежие
    }
}

static void warnLoop(const BmsState& b, float avgMv) {
    uint16_t ovp = 0, uvp = 0;
    uint16_t spreadMax = 50;
#if !defined(NO_BLE)
    BmsLimits l = dalyLimits();
    if (l.have) { ovp = l.cellOvProt; uvp = l.cellUvProt; if (l.r164 >= 20 && l.r164 <= 300) spreadMax = l.r164; }
#endif
    char t[72];
    bool chg = b.current > 1;
    if (ovp) {
        snprintf(t, sizeof(t), "ячейка %.3f В — у защиты от перезаряда %.2f В", b.cellMaxMv / 1000.0f, ovp / 1000.0f);
        warnSet(0, b.cellMaxMv >= ovp - 20, true, t, 5, 60);
    }
    // «100 %» и «0 %» — пороги своего счёта (по умолчанию под Li-ion первого
    // устройства). Без него не проверять: у LiFePO4 HS35 ячейки ~3,3 В, и
    // «0 % при 3,40 В» горело бы всегда, как только ячейки придут по RS485.
    bool own = battSocOn();
    snprintf(t, sizeof(t), "ячейка %.3f В — выше 100 %% (%.2f В)", b.cellMaxMv / 1000.0f, cfg().bFullMv / 1000.0f);
    warnSet(1, own && b.cellMaxMv > cfg().bFullMv + 20, false, t, 30, 120);
    if (uvp) {
        snprintf(t, sizeof(t), "ячейка %.3f В — у защиты от разряда %.2f В", b.cellMinMv / 1000.0f, uvp / 1000.0f);
        warnSet(2, b.cellMinMv && b.cellMinMv <= uvp + 100, true, t, 5, 60);
    }
    snprintf(t, sizeof(t), "ячейка %.3f В — батарея на 0 %% (%.2f В)", b.cellMinMv / 1000.0f, cfg().bEmptyMv / 1000.0f);
    warnSet(3, own && b.cellMinMv && b.cellMinMv <= cfg().bEmptyMv, false, t, 30, 120);
    snprintf(t, sizeof(t), "разброс ячеек %u мВ (больше %u)", b.spreadMv(), spreadMax);
    warnSet(4, b.spreadMv() > spreadMax, false, t, 600, 300);
    snprintf(t, sizeof(t), "заряд при %d °C — %s", b.tempMin, b.tempMin < 0 ? "ниже нуля литий заряжать нельзя" : "холодно, лучше 3,95 В на ячейку");
    warnSet(5, chg && b.tempMin < 5, b.tempMin < 0, t, 60, 300);
    snprintf(t, sizeof(t), "батарея %d °C — горячо", b.tempMax);
    warnSet(6, b.tempMax > 45, b.tempMax > 55, t, 60, 300);
    (void)avgMv;
}

String battWarnJson() {
    String j = "[";
    bool first = true;
    for (auto& w : s_w) {
        if (!w.on) continue;
        if (!first) j += ",";
        first = false;
        j += "\"" + jsonEsc(w.text) + "\"";
    }
    return j + "]";
}

// ------------------------------------------------------------------- счёт

void battSocLoop() {
    uint32_t now = millis();
    if (now - s_lastMs < 1000) return;
    float dt = s_lastMs ? (now - s_lastMs) / 1000.0f : 0;
    s_lastMs = now;

    BmsState b = stateSnapshot();
    if (!b.fresh || !b.cellCount || b.cellCount > MAX_CELLS) return;   // без ячеек считать не от чего
    uint32_t sum = 0;
    for (uint8_t i = 0; i < b.cellCount; i++) sum += b.cellMv[i];
    float avg = (float)sum / b.cellCount;
    if (avg < 2000) return;                               // ячейки ещё не пришли

    warnLoop(b, avg);
    if (!battSocOn()) return;

    float cap = cfg().bCapAh;
    if (dt > 5) dt = 0;                                   // долгий пропуск — не интегрируем вслепую
    double dAh = b.current * dt / 3600.0;
    s_cum += dAh;
    if (s_ah < 0) {                                       // первый запуск — по напряжению
        s_ah = cap * ocvSoc(avg) / 100.0f;
        Serial.printf("[заряд] начало счёта по напряжению: %.0f %%\n", ocvSoc(avg));
    } else {
        s_ah += dAh;
    }

    // 100 %: средняя ячейка у своей «полной» 10 с.
    if (avg >= cfg().bFullMv) {
        if (!s_fullSince) s_fullSince = now;
        if (!s_isFull && now - s_fullSince > 10000) {
            s_isFull = true;
            if (s_anchor == ANC_EMPTY) addMeas((float)(s_cum - s_anchorCum), "от пустой до полной");
            s_anchor = ANC_FULL; s_anchorCum = s_cum;
            s_ah = cap;
            evAdd(EV_BATT_FULL);
            save();
        }
    } else { s_fullSince = 0; if (avg < cfg().bFullMv - 30) s_isFull = false; }

    // 0 %: средняя ячейка на «пустой» 10 с (под нагрузкой — так и задумано).
    if (avg <= cfg().bEmptyMv) {
        if (!s_emptySince) s_emptySince = now;
        if (!s_isEmpty && now - s_emptySince > 10000) {
            s_isEmpty = true;
            if (s_anchor == ANC_FULL) addMeas((float)(s_anchorCum - s_cum), "от полной до пустой");
            s_anchor = ANC_EMPTY; s_anchorCum = s_cum;
            s_ah = 0;
            evAdd(EV_BATT_EMPTY);
            save();
        }
    } else { s_emptySince = 0; if (avg > cfg().bEmptyMv + 50) s_isEmpty = false; }

    // Покой: ток меньше 1 % ёмкости 15 минут — напряжение честное.
    if (fabsf(b.current) < cap * 0.01f) {
        if (!s_restSince) s_restSince = now;
        if (now - s_restSince > 900000UL) {
            s_restSince = now;                            // следующая сверка — через 15 мин
            float o = ocvSoc(avg), m = s_ah * 100 / cap;
            if (fabsf(o - m) > 10) {
                Serial.printf("[заряд] покой: счёт %.0f %%, по напряжению %.0f %% — поправляю\n", m, o);
                s_ah = cap * o / 100;
            }
        }
    } else s_restSince = 0;

    if (s_ah < 0) s_ah = 0;
    if (s_ah > cap) s_ah = cap;
    float ah = s_ah;
    stateUpdate([&](BmsState& x) {
        x.soc = ah * 100 / cap;
        x.socKnown = true;
        x.capAh = cap;
        x.remainingMah = (uint32_t)(ah * 1000);
    });
    if (now - s_saveMs > 600000UL) save();                // NVS — раз в 10 мин
}

String battSocSet(float capAh, uint16_t fullMv, uint16_t emptyMv) {
    if (capAh != 0 && (capAh < 5 || capAh > 2000)) return "ёмкость — от 5 до 2000 А·ч (0 — выключить свой счёт)";
    if (fullMv < 3300 || fullMv > 4400) return "100 % — от 3300 до 4400 мВ на ячейку";
    if (emptyMv < 2500 || emptyMv + 300 > fullMv) return "0 % — от 2500 мВ и хотя бы на 300 мВ ниже 100 %";
    float old = cfg().bCapAh;
    cfg().bCapAh = capAh;
    cfg().bFullMv = fullMv;
    cfg().bEmptyMv = emptyMv;
    cfgSave();
    // Процент сохраняем при смене ёмкости: остаток пересчитывается в новых А·ч.
    if (old > 1 && capAh > 1 && s_ah >= 0) s_ah = s_ah / old * capAh;
    if (capAh <= 1) s_ah = -1;
    save();
    return "";
}

String battSocSetPct(float pct) {
    if (!battSocOn()) return "сначала задайте ёмкость";
    if (pct < 0 || pct > 100) return "процент — от 0 до 100";
    s_ah = cfg().bCapAh * pct / 100;
    save();
    return "";
}

String battSocJson() {
    char b[96];
    String j = "{\"on\":" + String(battSocOn() ? "true" : "false");
    snprintf(b, sizeof(b), ",\"cap\":%.1f,\"full\":%u,\"empty\":%u,\"ah\":%.2f", cfg().bCapAh, cfg().bFullMv, cfg().bEmptyMv, s_ah);
    j += b;
    j += ",\"anchor\":\"" + String(s_anchor == ANC_FULL ? "full" : s_anchor == ANC_EMPTY ? "empty" : "none") + "\"";
    snprintf(b, sizeof(b), ",\"since\":%.2f", s_anchor ? (float)(s_cum - s_anchorCum) : 0.0f);
    j += b;
    j += ",\"meas\":[";
    for (uint8_t i = 0; i < s_measN; i++) j += (i ? "," : "") + String(s_meas[i], 1);
    j += "]";
#if !defined(NO_BLE)
    BmsLimits l = dalyLimits();
    if (l.have) {
        String o = ",\"lim\":{";
        snprintf(b, sizeof(b), "\"rated\":%.1f,\"ovw\":%u,\"ovp\":%u,\"uvw\":%u,\"uvp\":%u,", l.ratedAh, l.cellOvWarn, l.cellOvProt, l.cellUvWarn, l.cellUvProt);
        o += b;
        snprintf(b, sizeof(b), "\"povw\":%.1f,\"povp\":%.1f,\"puvw\":%.1f,\"puvp\":%.1f,", l.packOvWarn, l.packOvProt, l.packUvWarn, l.packUvProt);
        o += b;
        snprintf(b, sizeof(b), "\"dw\":%.1f,\"dp\":%.1f,\"cw\":%.1f,\"cp\":%.1f,\"r164\":%u,\"age\":%lu}", l.dsgWarnA, l.dsgProtA, l.chgWarnA, l.chgProtA, l.r164, (unsigned long)((millis() - l.ms) / 1000));
        o += b;
        j += o;
    }
#endif
    return j + "}";
}
