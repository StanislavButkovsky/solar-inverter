// Расписание режимов. Описание — в schedule.h.
#include "schedule.h"
#include "profile.h"
#include "state.h"
#include "weather.h"
#include "tg.h"
#include "stats.h"
#include <Preferences.h>
#include <time.h>

static const uint8_t NRULE = 8;
static const int32_t TZ_SEC = 3 * 3600;                  // как везде в прошивке

// d — дни недели битами, пн = бит 0 … вс = бит 6. w — 0 OPP, 1 CHP.
struct Rule { uint8_t on, d, h, m, w, c; };
static Rule    s_r[NRULE];
static uint8_t s_n = 0;
static int32_t s_fired[NRULE];                           // местные сутки, когда сработало
// Ждущие переключения (что, код): инвертор занят — повторяем до 5 минут.
// Их бывает два: после сохранения расписания — и OPP, и CHP сразу.
static int8_t  s_qW[2] = { -1, -1 }, s_qC[2];
static uint32_t s_qMs[2];
static volatile bool s_catchUp = false;                  // сохранили — применить то, что действует сейчас

static void enqueue(uint8_t w, uint8_t c) {
    for (uint8_t i = 0; i < 2; i++)
        if (s_qW[i] < 0 || s_qW[i] == w) { s_qW[i] = w; s_qC[i] = c; s_qMs[i] = millis(); return; }
}

static const char* const NAME[2][3] = { { "GRD", "PU", "PBG" }, { "PV", "PNG", "OPV" } };

static void save() {
    Preferences p;
    if (!p.begin("sched", false)) return;
    p.putBytes("r", s_r, sizeof(Rule) * s_n);
    p.end();
}

void schedStart() {
    Preferences p;
    if (p.begin("sched", true)) {
        size_t n = p.getBytesLength("r") / sizeof(Rule);
        if (n > NRULE) n = NRULE;
        if (n) p.getBytes("r", s_r, n * sizeof(Rule));
        s_n = n;
        p.end();
    }
    for (uint8_t i = 0; i < NRULE; i++) s_fired[i] = -1;
}

static String ruleText(const Rule& r) {
    static const char* const DAY[] = { "пн", "вт", "ср", "чт", "пт", "сб", "вс" };
    String d;
    if (r.d == 0x7F) d = "каждый день";
    else if (r.d == 0x1F) d = "по будням";
    else if (r.d == 0x60) d = "по выходным";
    else for (uint8_t i = 0; i < 7; i++) if (r.d >> i & 1) d += String(d.length() ? "," : "") + DAY[i];
    char t[8];
    snprintf(t, sizeof(t), "%02u:%02u", r.h, r.m);
    return d + " в " + t + " — " + (r.w ? "CHP " : "OPP ") + NAME[r.w][r.c < 3 ? r.c : 0];
}

void schedLoop() {
    static uint32_t last = 0;
    if (millis() - last < 5000) return;
    last = millis();
    if (!statsTimeValid()) return;
    time_t loc = time(nullptr) + TZ_SEC;
    struct tm lt;
    gmtime_r(&loc, &lt);
    int32_t day = loc / 86400;
    uint8_t wd = (lt.tm_wday + 6) % 7;                   // пн = 0

    // Ждущие переключения: инвертор был занят — пробуем ещё, но не дольше 5 минут.
    for (uint8_t i = 0; i < 2; i++) {
        if (s_qW[i] < 0) continue;
        String e = invSetSetting(s_qW[i], s_qC[i]);
        if (e.isEmpty()) s_qW[i] = -1;
        else if (millis() - s_qMs[i] > 300000) {
            tgNotify("⏰ Расписание: не удалось поставить " + String(s_qW[i] ? "CHP " : "OPP ") + NAME[s_qW[i]][s_qC[i]] + " — " + e);
            s_qW[i] = -1;
        }
        return;                                           // одно переключение за раз
    }

    // Только что сохранили расписание: поставить то, что по нему действует
    // сейчас — последнее прошедшее правило для OPP и для CHP. 25.10.2026
    // расписание «22:00 GRD / 08:00 PBG» сохранили в 23:18, и до утра ничего не
    // происходило: правило срабатывало только в свою минуту.
    if (s_catchUp) {
        s_catchUp = false;
        int nowMin = lt.tm_hour * 60 + lt.tm_min;
        InvState v = invSnapshot();
        String said;
        for (uint8_t w = 0; w < 2; w++) {
            int best = -1, bestAgo = 1 << 30;
            for (uint8_t i = 0; i < s_n; i++) {
                const Rule& r = s_r[i];
                if (!r.on || r.w != w) continue;
                int rm = r.h * 60 + r.m;
                for (int back = 0; back <= 7; back++) {
                    uint8_t d = (wd + 7 - back) % 7;
                    if (!(r.d >> d & 1) || (back == 0 && rm > nowMin)) continue;
                    int ago = back * 1440 + nowMin - rm;
                    if (ago < bestAgo) { bestAgo = ago; best = i; }
                    break;
                }
            }
            if (best < 0) continue;
            const Rule& r = s_r[best];
            if (w == 1 && weatherChargeActive()) { said += "\nCHP — не трогаю, идёт ночная зарядка"; continue; }
            uint8_t now = w ? v.prioChg : v.prioOut;
            if (v.fresh && now == r.c) { said += String("\n") + (w ? "CHP " : "OPP ") + NAME[w][r.c] + " — уже стоит"; continue; }
            said += String("\n") + (w ? "CHP → " : "OPP → ") + NAME[w][r.c] + " (правило " + ruleText(r) + ")";
            enqueue(w, r.c);
        }
        if (said.length()) tgNotify("⏰ Расписание сохранено, сейчас по нему:" + said);
        return;
    }
    for (uint8_t i = 0; i < s_n; i++) {
        const Rule& r = s_r[i];
        if (!r.on || !(r.d >> wd & 1) || r.h != lt.tm_hour || r.m != lt.tm_min || s_fired[i] == day) continue;
        s_fired[i] = day;
        if (r.w == 1 && weatherChargeActive()) {
            tgNotify("⏰ Расписание: " + ruleText(r) + " — пропущено, идёт ночная зарядка по прогнозу.");
            continue;
        }
        InvState v = invSnapshot();
        uint8_t now = r.w ? v.prioChg : v.prioOut;
        if (v.fresh && now == r.c) continue;              // уже стоит — молча
        tgNotify("⏰ По расписанию: " + String(r.w ? "CHP" : "OPP") + " → " + NAME[r.w][r.c]);
        String e = invSetSetting(r.w, r.c);
        if (e.length()) enqueue(r.w, r.c);
        return;                                           // одно переключение за раз
    }
}

String schedJson() {
    String j = "{\"rules\":[";
    for (uint8_t i = 0; i < s_n; i++) {
        const Rule& r = s_r[i];
        if (i) j += ",";
        j += "{\"on\":" + String(r.on) + ",\"d\":" + String(r.d) + ",\"h\":" + String(r.h) + ",\"m\":" + String(r.m) +
             ",\"w\":" + String(r.w) + ",\"c\":" + String(r.c) + "}";
    }
    return j + "]}";
}

String schedSave(const String& all) {
    Rule r[NRULE];
    uint8_t n = 0;
    int from = 0;
    while (from < (int)all.length()) {
        int end = all.indexOf(';', from);
        if (end < 0) end = all.length();
        String one = all.substring(from, end);
        from = end + 1;
        if (!one.length()) continue;
        if (n >= NRULE) return "не больше " + String(NRULE) + " правил";
        unsigned on, d, h, m, w, c;
        if (sscanf(one.c_str(), "%u.%u.%u.%u.%u.%u", &on, &d, &h, &m, &w, &c) != 6) return "не разобрал правило " + String(n + 1);
        if (on > 1 || d == 0 || d > 0x7F || h > 23 || m > 59 || w > 1 || c > 2)
            return "правило " + String(n + 1) + ": проверьте дни, время и режим";
        r[n++] = { (uint8_t)on, (uint8_t)d, (uint8_t)h, (uint8_t)m, (uint8_t)w, (uint8_t)c };
    }
    memcpy(s_r, r, sizeof(Rule) * n);
    s_n = n;
    for (uint8_t i = 0; i < NRULE; i++) s_fired[i] = -1;
    // Только что сохранённое правило на текущую минуту не должно сработать
    // само собой посреди настройки — считаем эту минуту отработанной.
    if (statsTimeValid()) {
        time_t loc = time(nullptr) + TZ_SEC;
        struct tm lt;
        gmtime_r(&loc, &lt);
        for (uint8_t i = 0; i < n; i++) if (s_r[i].h == lt.tm_hour && s_r[i].m == lt.tm_min) s_fired[i] = loc / 86400;
    }
    s_qW[0] = s_qW[1] = -1;
    s_catchUp = true;
    save();
    return "";
}

String schedText() {
    if (!s_n) return "⏰ Расписания нет. Задаётся на странице: «Настройки» → «Авто».";
    String r = "⏰ Расписание режимов";
    for (uint8_t i = 0; i < s_n; i++) r += String("\n") + (s_r[i].on ? "• " : "⏸ ") + ruleText(s_r[i]);
    return r;
}
