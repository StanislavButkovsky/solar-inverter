// Расписание режимов. Описание — в schedule.h.
#include "schedule.h"
#include "profile.h"
#include "state.h"
#include "weather.h"
#include "tg.h"
#include "stats.h"
#include "cfg.h"
#include <Preferences.h>
#include <time.h>

static const uint8_t NRULE = 8;
static const int32_t TZ_SEC = 3 * 3600;                  // как везде в прошивке

// d — дни недели битами, пн = бит 0 … вс = бит 6. w — 0 OPP, 1 CHP.
// k — от чего время: 0 часы (h:m), 1 восход, 2 закат; o — сдвиг от них в
// минутах (минус — раньше). В NVS ключ "r2"; "r" — прежние правила без k и
// o (6 байт), их забирает schedStart: от старой прошивки или из старой копии.
struct Rule { uint8_t on, d, h, m, w, c, k, _r; int16_t o; };
struct RuleV1 { uint8_t on, d, h, m, w, c; };
static const int16_t OFS_MAX = 360;                      // сдвиг — не больше 6 ч
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
    p.putBytes("r2", s_r, sizeof(Rule) * s_n);
    p.remove("r");
    p.end();
}

void schedStart() {
    Preferences p;
    bool old = false;
    if (p.begin("sched", true)) {
        size_t n = p.getBytesLength("r") / sizeof(RuleV1);
        old = p.isKey("r");
        if (old) {
            RuleV1 v[NRULE];
            if (n > NRULE) n = NRULE;
            if (n) p.getBytes("r", v, n * sizeof(RuleV1));
            for (size_t i = 0; i < n; i++) s_r[i] = { v[i].on, v[i].d, v[i].h, v[i].m, v[i].w, v[i].c, 0, 0, 0 };
        } else {
            n = p.getBytesLength("r2") / sizeof(Rule);
            if (n > NRULE) n = NRULE;
            if (n) p.getBytes("r2", s_r, n * sizeof(Rule));
        }
        s_n = n;
        p.end();
    }
    if (old) save();                                     // перевести в новый вид один раз
    for (uint8_t i = 0; i < NRULE; i++) s_fired[i] = -1;
}

// Восход и закат в местных минутах от полуночи для местных суток day (дни
// от 1970). Уравнение восхода (Wikipedia «Sunrise equation»), с рефракцией
// −0,833°; по Москве сходится с таблицами до 1–2 мин. false — координат нет
// или солнце сегодня не садится/не встаёт.
static bool sunTimes(int32_t day, int& rise, int& set) {
    float lat = cfg().lat, lon = cfg().lon;
    if (lat == 0 && lon == 0) return false;
    const double R = M_PI / 180;
    double j = day - 10957 + 0.0008 - lon / 360;           // дни от полудня 1.1.2000
    double M = fmod(357.5291 + 0.98560028 * j, 360);
    double C = 1.9148 * sin(M * R) + 0.02 * sin(2 * M * R) + 0.0003 * sin(3 * M * R);
    double L = fmod(M + C + 180 + 102.9372, 360);
    double jt = j + 0.0053 * sin(M * R) - 0.0069 * sin(2 * L * R);
    double sd = sin(L * R) * sin(23.4397 * R), cd = cos(asin(sd));
    double cw = (sin(-0.833 * R) - sin(lat * R) * sd) / (cos(lat * R) * cd);
    if (cw < -1 || cw > 1) return false;
    double w = acos(cw) / R / 360;
    auto mins = [&](double t) { return (int)lround((t - (day - 10957) + 0.5) * 1440 + TZ_SEC / 60); };
    rise = mins(jt - w);
    set = mins(jt + w);
    return true;
}

// Во сколько правило срабатывает в эти местные сутки; −1 — не считается.
static int ruleMin(const Rule& r, int32_t day) {
    if (r.k == 0) return r.h * 60 + r.m;
    int rise, set;
    if (!sunTimes(day, rise, set)) return -1;
    int t = (r.k == 1 ? rise : set) + r.o;
    return t < 0 ? 0 : t > 1439 ? 1439 : t;
}

static int32_t today() { return (time(nullptr) + TZ_SEC) / 86400; }

static String hhmm(int t) {
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", t / 60, t % 60);
    return b;
}

static String ruleText(const Rule& r) {
    static const char* const DAY[] = { "пн", "вт", "ср", "чт", "пт", "сб", "вс" };
    String d;
    if (r.d == 0x7F) d = "каждый день";
    else if (r.d == 0x1F) d = "по будням";
    else if (r.d == 0x60) d = "по выходным";
    else for (uint8_t i = 0; i < 7; i++) if (r.d >> i & 1) d += String(d.length() ? "," : "") + DAY[i];
    String when;
    if (r.k == 0) when = " в " + hhmm(r.h * 60 + r.m);
    else {
        const char* ev = r.k == 1 ? "восхода" : "заката";
        int a = abs(r.o);
        String span = a >= 60 ? String(a / 60) + " ч" + (a % 60 ? " " + String(a % 60) + " мин" : "") : String(a) + " мин";
        when = r.o == 0 ? String(r.k == 1 ? " на восходе" : " на закате")
             : r.o < 0 ? " за " + span + " до " + ev : " через " + span + " после " + ev;
        int t = statsTimeValid() ? ruleMin(r, today()) : -1;
        if (t >= 0) when += " (сегодня " + hhmm(t) + ")";
    }
    return d + when + " — " + (r.w ? "CHP " : "OPP ") + NAME[r.w][r.c < 3 ? r.c : 0];
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
                for (int back = 0; back <= 7; back++) {
                    uint8_t d = (wd + 7 - back) % 7;
                    int rm = ruleMin(r, day - back);
                    if (rm < 0 || !(r.d >> d & 1) || (back == 0 && rm > nowMin)) continue;
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
        if (!r.on || !(r.d >> wd & 1) || ruleMin(r, day) != lt.tm_hour * 60 + lt.tm_min || s_fired[i] == day) continue;
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
             ",\"w\":" + String(r.w) + ",\"c\":" + String(r.c) + ",\"k\":" + String(r.k) + ",\"o\":" + String(r.o) + "}";
    }
    // Восход и закат сегодня — странице, чтобы показать время правила до сохранения.
    int rise, set;
    if (statsTimeValid() && sunTimes(today(), rise, set))
        j += "],\"rise\":" + String(rise) + ",\"set\":" + String(set) + "}";
    else j += "],\"rise\":-1,\"set\":-1}";
    return j;
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
        // "on.дни.ч.м.что.код[.от чего.сдвиг]" — без двух последних — по часам
        unsigned on, d, h, m, w, c, k = 0;
        int o = 0;
        int got = sscanf(one.c_str(), "%u.%u.%u.%u.%u.%u.%u.%d", &on, &d, &h, &m, &w, &c, &k, &o);
        if (got != 6 && got != 8) return "не разобрал правило " + String(n + 1);
        if (on > 1 || d == 0 || d > 0x7F || h > 23 || m > 59 || w > 1 || c > 2 || k > 2 || o < -OFS_MAX || o > OFS_MAX)
            return "правило " + String(n + 1) + ": проверьте дни, время и режим";
        if (k && cfg().lat == 0 && cfg().lon == 0)
            return "для правил от восхода и заката задайте координаты в «Погоде» выше";
        if (!k) o = 0;
        r[n++] = { (uint8_t)on, (uint8_t)d, (uint8_t)h, (uint8_t)m, (uint8_t)w, (uint8_t)c, (uint8_t)k, 0, (int16_t)o };
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
        for (uint8_t i = 0; i < n; i++) if (ruleMin(s_r[i], loc / 86400) == lt.tm_hour * 60 + lt.tm_min) s_fired[i] = loc / 86400;
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
