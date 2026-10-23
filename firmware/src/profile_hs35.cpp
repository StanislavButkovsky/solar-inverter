// Профиль второго инвертора: Hiden Control HS35-5648PRO по USB.
//
// Ждали Modbus, как у MUST PV18, а разведки 19.10.2026 показали: за USB мост
// CH340, порт на 9600 8N1 разбирает строки до \r, отвечает голым «NAK\r»
// на незнакомое и принимает команду только побайтно с паузой от 5 мс.
//
// Это протокол JsdSolar «G-command ASCII»: им говорят инверторы JSD через
// датлоггеры EyeBond (Wi-Fi-модуль Hiden — EyeBond с приложением SmartClient).
// Описан в groove-max/ha-eybond-local. 20.10.2026 живой HS35 ответил на все
// команды: GPDAT0, GPV, GOP, GLINE, GBAT, BL, GMOD, GWS, GTMP, GBMS, GCHG, F,
// SVFW, Q1.
//
// Главная особенность: КАЖДЫЙ ОТВЕТ ПРИХОДИТ НА ОДНУ КОМАНДУ ПОЗЖЕ. На GPV
// пришёл ответ GPDAT0, на SVFW — номиналы F и так по всей цепочке. Поэтому
// ответ опознаётся по содержимому, а не по тому, что спросили последним:
// у каждого свой отпечаток — число полей и вид первого.
#if defined(TARGET_HS35) && !defined(TARGET_ASTERION) && !defined(TARGET_MUST)

#include "profile.h"
#include "state.h"
#include "hs35_settings.h"
#include <Preferences.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

const char*    INV_PROFILE_NAME = "JsdSolar G-ASCII (HS35 по USB)";
const uint32_t INV_BAUD   = 9600;
const uint16_t INV_GAP_MS = 10;          // ответ с 5 мс, берём с запасом
const uint8_t  INV_NSTEPS = 1;           // круг ведёт сам профиль

// ------------------------------------------------------------------ команды

enum Kind : uint8_t {
    K_GPDAT0, K_GPV, K_GOP, K_GLINE, K_GBAT, K_BL, K_GMOD, K_GWS, K_GTMP, K_GBMS,
    K_GCHG, K_F, K_SVFW, K_Q1,
    K_OPR, K_CPR, K_OPM, K_TBAT,                 // настройки: чтение
    K_COUNT,
    K_WOPR = 0xF0, K_WCPR,                       // запись OPRdd / CPRdd — в очереди, не в круге
    K_WGEN,                                      // запись остальных настроек (hs35_settings.h)
    // 0x80 + i — чтение настройки SETS[i]; 0x80 + SET_TE/SET_TD — флаги TE?/TD?
    K_ACK = 0xFC, K_SHORT = 0xFD, K_NAK = 0xFE, K_UNKNOWN = 0xFF
};
static const char* const NAME[K_COUNT] = {
    "GPDAT0", "GPV", "GOP", "GLINE", "GBAT", "BL", "GMOD", "GWS", "GTMP", "GBMS",
    "GCHG", "F", "SVFW", "Q1",
    "OPR??", "CPR??", "OPM??", "TBAT?",
};

// Живые данные через шаг, остальное по кругу. Номиналы и версия — изредка.
static const Kind CYCLE[] = {
    K_GPDAT0, K_GPV,   K_GPDAT0, K_GOP,  K_GPDAT0, K_GLINE, K_GPDAT0, K_GBMS,
    K_GPDAT0, K_BL,    K_GPDAT0, K_GWS,  K_GPDAT0, K_GMOD,  K_GPDAT0, K_GTMP,
    K_GPDAT0, K_F,     K_GPDAT0, K_SVFW,
    K_GPDAT0, K_OPR,   K_GPDAT0, K_CPR,  K_GPDAT0, K_OPM,   K_GPDAT0, K_TBAT,
};
static uint8_t s_pos = 0;
static uint8_t s_sent = 0;               // команд с последнего ответа G-ASCII
static bool    s_gSeen = false;          // хоть один ответ G-ASCII был

static char   s_buf[200];
static size_t s_len = 0;

// Очередь отправленных команд. Ответы HS35 приходят на команду позже, а
// ответы на чтение настроек — «(00», «(01» — у OPR??, CPR?? и OPM?? одинаковы
// по виду. Поэтому каждый ответ снимает одну команду из очереди, а ответ
// однозначного вида ещё и выравнивает её: всё до своей команды — отвечено
// или потеряно. Короткий «(dd» принимается, только если в голове очереди
// стоит команда настроек.
// Журнал обмена: последние строки «→ команда» и «← ответ [чему приписан]»,
// по порядку. По нему видно сдвиг ответов и правильность очереди —
// понадобился, когда CPR?? прочитался не так, как на экране инвертора.
static String  s_trace[40];
static uint8_t s_tHead = 0, s_tLen = 0;
static void trace(const String& line) {
    s_trace[(s_tHead + s_tLen) % 40] = String(millis() / 100 % 10000) + " " + line;
    if (s_tLen < 40) s_tLen++; else s_tHead = (s_tHead + 1) % 40;
}

static const uint8_t SET_TE = 100, SET_TD = 101;     // флаги: включённые и выключенные

static const char* kname(uint8_t k) {
    if (k < K_COUNT) return NAME[k];
    if (k == K_WOPR) return "запись OPR";
    if (k == K_WCPR) return "запись CPR";
    if (k == K_WGEN) return "запись настройки";
    if (k >= 0x80 && k < 0x80 + NSETS) return SETS[k - 0x80].rd ? SETS[k - 0x80].rd : "?";
    if (k == 0x80 + SET_TE) return "TE?";
    if (k == 0x80 + SET_TD) return "TD?";
    return "?";
}

static Kind    s_fifo[8];
static uint8_t s_fHead = 0, s_fLen = 0;
static void fifoPush(Kind k) {
    if (s_fLen == 8) { s_fHead = (s_fHead + 1) % 8; s_fLen--; }   // переполнение — забываем старую
    s_fifo[(s_fHead + s_fLen) % 8] = k;
    s_fLen++;
}
static Kind fifoPop() {
    if (!s_fLen) return K_UNKNOWN;
    Kind k = s_fifo[s_fHead];
    s_fHead = (s_fHead + 1) % 8;
    s_fLen--;
    return k;
}
// Выровнять по однозначному ответу k: снять всё до него включительно.
static void fifoSync(Kind k) {
    for (uint8_t i = 0; i < s_fLen; i++)
        if (s_fifo[(s_fHead + i) % 8] == k) { for (uint8_t j = 0; j <= i; j++) fifoPop(); return; }
}

// Первый ответ каждого вида — в NVS, для status дома.
static Preferences s_nvs;
static bool   s_seen[K_COUNT];
static String s_report;

// Паспорт HS35-5648PRO: 5600 ВА, батарея 48 В. Ответ F их уточняет.
static float s_ratedVA = 5600, s_ratedBattV = 48;
static float s_capAh = 0;                // полная ёмкость батареи по BMS

// Сброс перед запросом не нужен и вреден: ответ запаздывает на команду,
// и строка, начатая до запроса, — это и есть нужный ответ. Строки и так
// разделены \r.
void invResetStream() { }

// ------------------------------------------------------------------ запись

// Переключение приоритетов. Коды — только сверенные на живом HS35
// 21.10.2026: OPP GRD 0, PU 1, PBG 2; CHP PV 0, PNG 1, OPV 2. Команда
// OPRdd / CPRdd уходит вне круга, ответ ACK или NAK сопоставляется через ту
// же очередь отправленных (он тоже приходит на команду позже), потом
// настройка перечитывается — «встало» говорит только перечитанное значение.
enum WState : uint8_t { W_IDLE, W_QUEUED, W_SENT, W_READ, W_VERIFY, W_DONE };
static const char* const W_NAME[] = { "OPP", "CHP" };
static const char* const W_OPT[2][3] = { { "GRD", "PU", "PBG" }, { "PV", "PNG", "OPV" } };
static volatile WState s_w = W_IDLE;
static volatile uint8_t s_wWhat = 0, s_wCode = 0;
static uint32_t s_wMs = 0, s_wSeq = 0;
static bool     s_wOk = false;
static String   s_wMsg;

static bool wBusy();
static bool gBusy();

static void wFinish(bool ok, const String& msg) {
    s_wOk = ok;
    s_wMsg = String(W_NAME[s_wWhat]) + " → " + W_OPT[s_wWhat][s_wCode] + ": " + msg;
    s_w = W_DONE;
    s_wSeq++;
}

String invSetSetting(uint8_t what, uint8_t code) {
    // Запись OPP сверена с экраном 21.10.2026 (/opptest): коды те же, что
    // при чтении. После PBG инвертор ещё пару минут работает от сети и лишь
    // потом уходит на батарею — это не откат, а его собственная выдержка.
    if (what > 1 || code > 2) return "такого значения нет";
    if (wBusy() || gBusy()) return "предыдущее переключение ещё идёт";
    s_wWhat = what;
    s_wCode = code;
    s_wMsg = "";
    s_w = W_QUEUED;
    return "";
}

// ------------------------------------------------------------------ остальные настройки

// Таблица — hs35_settings.h. Читаются вставкой в круг: каждая четвёртая
// команда — одна настройка, вся таблица обходится примерно за минуту.
// Ответ у всех одинакового вида — «(56.4», «(030», «(SIQ», — поэтому он
// приписывается по голове очереди отправленных, как «(dd» у OPR??.
// Настройка, на которую инвертор дважды ответил NAK, больше не спрашивается.
//
// Запись — только сверенной с экраном (отметка на странице, в NVS),
// в пределах руководства и с перечитыванием, как у OPP/CHP.
static float    s_sv[NSETS];             // значение в единицах страницы; NAN — неизвестно
static int8_t   s_sup[NSETS];            // −1 неизвестно, 0 инвертор не знает, 1 отвечает
static uint8_t  s_nak[NSETS];
static String   s_te, s_td;              // буквы включённых и выключенных флагов
static bool     s_teKnown = false, s_tdKnown = false;
static uint8_t  s_teNak = 0, s_tdNak = 0;
static uint64_t s_ver = 0;               // сверено с панелью — бит на настройку
static bool     s_setInit = false;
static uint8_t  s_setPos = 0, s_setTick = 0;

static int8_t setIdx(const char* id) {
    for (uint8_t i = 0; i < NSETS; i++) if (!strcmp(SETS[i].id, id)) return i;
    return -1;
}
static float setVal(const char* id) { int8_t i = setIdx(id); return i < 0 ? NAN : s_sv[i]; }

static void setInit() {
    if (s_setInit) return;
    s_setInit = true;
    for (uint8_t i = 0; i < NSETS; i++) { s_sv[i] = NAN; s_sup[i] = -1; s_nak[i] = 0; }
    if (s_nvs.begin("invprof", true)) { s_ver = s_nvs.getULong64("ver", 0); s_nvs.end(); }
}

// OPM?? и TBAT? и так в основном круге — их таблица не спрашивает отдельно.
static bool inCycle(uint8_t i) { return !strcmp(SETS[i].id, "mod") || !strcmp(SETS[i].id, "bat"); }

static float setScale(uint8_t i) { return SETS[i].scale > 0 ? SETS[i].scale : 1; }

// Следующая настройка для круга: команда и что положить в очередь.
static const char* nextSetRead(uint8_t& kind) {
    setInit();
    for (uint8_t tries = 0; tries < NSETS + 2; tries++) {
        uint8_t i = s_setPos;
        s_setPos = (s_setPos + 1) % (NSETS + 2);
        if (i == NSETS)     { if (s_teNak < 2) { kind = 0x80 + SET_TE; return "TE?"; } continue; }
        if (i == NSETS + 1) { if (s_tdNak < 2) { kind = 0x80 + SET_TD; return "TD?"; } continue; }
        if (!SETS[i].rd || inCycle(i) || s_sup[i] == 0) continue;
        kind = 0x80 + i;
        return SETS[i].rd;
    }
    return nullptr;
}

static void flagsUpdate() {
    for (uint8_t i = 0; i < NSETS; i++) {
        if (SETS[i].kind != SK_FLAG) continue;
        bool on = s_teKnown && s_te.indexOf(SETS[i].flag) >= 0;
        bool off = s_tdKnown && s_td.indexOf(SETS[i].flag) >= 0;
        if (on || off) { s_sv[i] = on ? 1 : 0; s_sup[i] = 1; }
        else if (s_teKnown && s_tdKnown) s_sup[i] = 0;          // буквы нет ни там, ни там
    }
}

// Запись: своё состояние, но общие с OPP/CHP «занято» и итог (s_wSeq/s_wMsg),
// чтобы журнал и бот узнавали о ней тем же путём.
enum GState : uint8_t { G_IDLE, G_QUEUED, G_SENT, G_READ, G_VERIFY, G_DONE };
static volatile GState s_g = G_IDLE;
static uint8_t  s_gIdx = 0;
static float    s_gWant = 0;
static char     s_gCmd[20];
static uint32_t s_gMs = 0;

static bool gBusy() { return s_g == G_QUEUED || s_g == G_SENT || s_g == G_READ || s_g == G_VERIFY; }
static bool wBusy() { return s_w == W_QUEUED || s_w == W_SENT || s_w == W_READ || s_w == W_VERIFY; }

static String fmtVal(uint8_t i, float v) {
    const SetDef& d = SETS[i];
    if (isnan(v)) return "—";
    if (d.kind == SK_FLAG) return v > 0.5f ? "вкл." : "выкл.";
    if (d.kind == SK_ENUM && d.opts) {
        String o = d.opts;
        int n = (int)lroundf(v), from = 0;
        for (int k = 0; k < n && from >= 0; k++) { from = o.indexOf('|', from); if (from >= 0) from++; }
        if (from < 0) return String(n);
        int to = o.indexOf('|', from);
        String r = o.substring(from, to < 0 ? o.length() : to);
        int dash = r.indexOf(" — ");
        return dash > 0 ? r.substring(0, dash) : r;
    }
    String r = String(v, (unsigned)d.dec);
    r.replace('.', ',');
    return d.unit[0] ? r + " " + d.unit : r;
}

static void gFinish(bool ok, const String& msg) {
    const SetDef& d = SETS[s_gIdx];
    s_wOk = ok;
    s_wMsg = String(d.panel[0] ? d.panel : d.id) + " → " + fmtVal(s_gIdx, s_gWant) + ": " + msg;
    s_g = G_DONE;
    s_wSeq++;
}

static void applySetValue(uint8_t idx, const char* text) {
    setInit();
    if (idx == SET_TE || idx == SET_TD) {
        String t = text;
        t.trim();
        if (idx == SET_TE) { s_te = t; s_teKnown = true; } else { s_td = t; s_tdKnown = true; }
        flagsUpdate();
        if (s_g == G_VERIFY && SETS[s_gIdx].kind == SK_FLAG && idx == SET_TE) {
            bool on = s_te.indexOf(SETS[s_gIdx].flag) >= 0;
            if (on == (s_gWant > 0.5f)) gFinish(true, "встало");
            else gFinish(false, "инвертор ответил ACK, но флаг не сменился");
        }
        return;
    }
    if (idx >= NSETS || !text[0] || (!isdigit((uint8_t)text[0]) && text[0] != '-')) return;
    float v = strtof(text, nullptr) / setScale(idx);
    s_sv[idx] = v;
    s_sup[idx] = 1;
    if (s_g == G_VERIFY && s_gIdx == idx) {
        float tol = SETS[idx].dec ? 0.05f : 0.5f;
        if (fabsf(v - s_gWant) <= tol) gFinish(true, "встало");
        else gFinish(false, "инвертор ответил ACK, но значение " + fmtVal(idx, v));
    }
}

static void setNak(uint8_t idx) {
    setInit();
    if (idx == SET_TE) { if (s_teNak < 2) s_teNak++; return; }
    if (idx == SET_TD) { if (s_tdNak < 2) s_tdNak++; return; }
    if (idx >= NSETS) return;
    if (++s_nak[idx] >= 2) s_sup[idx] = 0;
    if (s_g == G_VERIFY && s_gIdx == idx) gFinish(false, "инвертор не ответил на перечитывание");
}

// Пределы для этой настройки сейчас — по типу батареи и режиму MOD.
// Пустая строка — можно; иначе причина, почему нельзя.
static String setLimits(uint8_t i, float& lo, float& hi) {
    const SetDef& d = SETS[i];
    lo = d.lo; hi = d.hi;
    float bt = setVal("bat"), mod = setVal("mod");
    switch (d.rule) {
    case SR_READONLY: return "только показываем";
    case SR_BATTV:
        if (isnan(bt)) return "тип батареи ещё не прочитан";
        if (bt < 1.5f) return "у AGM и FLD инвертор это напряжение менять не даёт";
        if (bt > 2.5f && (d.cLo || d.cHi)) { lo = d.cLo; hi = d.cHi; }
        break;
    case SR_EQ:
        if (isnan(bt)) return "тип батареи ещё не прочитан";
        if (fabsf(bt - 2) < 0.5f && d.kind != SK_FLAG) return "у литиевой батареи выравнивание не используется";
        break;
    case SR_LLV:
        if (isnan(mod)) return "режим MOD ещё не прочитан";
        if (mod > 0.5f) { lo = 170; hi = 200; }
        break;
    default: break;
    }
    if (!strcmp(d.id, "lhv")) {
        if (isnan(mod)) return "режим MOD ещё не прочитан";
        if (mod > 0.5f) return "меняется только в режиме APP";
    }
    return "";
}

// Почему нельзя писать вообще (без учёта значения) — для страницы.
static String setWhyNot(uint8_t i) {
    float lo, hi;
    String e = setLimits(i, lo, hi);
    if (e.length()) return e;
    if (!SETS[i].wr && SETS[i].kind != SK_FLAG) return "только показываем";
    if (s_sup[i] == 0) return "инвертор эту настройку не знает";
    if (s_sup[i] < 0 || isnan(s_sv[i])) return "ещё не прочитана";
    if (!(s_ver >> i & 1)) return "сначала сверьте с экраном инвертора (программа " + String(SETS[i].prog) + ")";
    return "";
}

// Взаимные условия: заряд выше поддерживающего, отключение ниже
// предупреждения и т. д. Неизвестное соседнее значение не мешает.
static String setCross(const char* id, float v) {
    struct Pair { const char* lowId; const char* highId; const char* text; };
    static const Pair P[] = {
        { "bfl", "bcv", "поддерживающий заряд должен быть ниже основного" },
        { "bau", "bal", "отключение батареи должно быть ниже предупреждения" },
        { "bau", "btg", "отключение батареи должно быть ниже порога ухода на сеть" },
        { "btg", "btb", "уход на сеть должен быть ниже возврата на батарею" },
        { "stg", "stb", "уход на сеть по заряду должен быть ниже возврата на батарею" },
        { "bsu", "stg", "отключение по заряду должно быть ниже ухода на сеть" },
    };
    for (const Pair& q : P) {
        if (!strcmp(id, q.lowId)) { float o = setVal(q.highId); if (!isnan(o) && v >= o) return q.text; }
        if (!strcmp(id, q.highId)) { float o = setVal(q.lowId); if (!isnan(o) && v <= o) return q.text; }
    }
    return "";
}

String invSettingWrite(const String& id, const String& value) {
    setInit();
    int8_t i = setIdx(id.c_str());
    if (i < 0) return "нет такой настройки";
    const SetDef& d = SETS[i];
    String e = setWhyNot(i);
    if (e.length()) return e;
    if (gBusy() || wBusy()) return "предыдущее переключение ещё идёт";
    if (!invSnapshot().fresh) return "инвертор сейчас не на связи";
    String t = value;
    t.trim();
    t.replace(',', '.');
    if (!t.length()) return "пустое значение";
    float v = t.toFloat();
    float lo, hi;
    setLimits(i, lo, hi);
    if (d.kind == SK_FLAG) {
        if (v != 0 && v != 1) return "только вкл. или выкл.";
        if (d.rule == SR_EQ && v > 0.5f && fabsf(setVal("bat") - 2) < 0.5f)
            return "у литиевой батареи выравнивающая зарядка вредна — не включаю";
        snprintf(s_gCmd, sizeof(s_gCmd), "T%c%c", v > 0.5f ? 'E' : 'D', d.flag);
    } else {
        if (d.list) {
            bool ok = false;
            String l = String("|") + d.list + "|";
            if (l.indexOf("|" + String((int)lroundf(v)) + "|") >= 0 && fabsf(v - lroundf(v)) < 0.01f) ok = true;
            if (!ok) return String("допустимо: ") + String(d.list).substring(0) + " " + d.unit;
        } else {
            if (v < lo - 0.001f || v > hi + 0.001f)
                return "допустимо " + fmtVal(i, lo) + " … " + fmtVal(i, hi) + " (по руководству)";
            float steps = (v - lo) / d.step;
            if (fabsf(steps - lroundf(steps)) > 0.01f) return "шаг " + String(d.step, (unsigned)d.dec) + " " + d.unit;
        }
        if (d.kind == SK_ENUM && (v < d.lo || v > d.hi)) return "такого варианта нет";
        String c = setCross(d.id, v);
        if (c.length()) return c;
        float raw = v * setScale(i);
        if (d.width) snprintf(s_gCmd, sizeof(s_gCmd), "%s%0*ld", d.wr, d.width, (long)lroundf(raw));
        else         snprintf(s_gCmd, sizeof(s_gCmd), "%s%.*f", d.wr, d.dec, raw);
    }
    if (fabsf(v - s_sv[i]) < 0.001f) return "уже стоит " + fmtVal(i, v);
    s_gIdx = i;
    s_gWant = v;
    s_wMsg = "";
    s_g = G_QUEUED;
    return "";
}

String invSettingVerify(const String& id, bool on) {
    setInit();
    int8_t i = setIdx(id.c_str());
    if (i < 0) return "нет такой настройки";
    if (on && isnan(s_sv[i])) return "ещё не прочитана — сверять не с чем";
    if (on) s_ver |= (uint64_t)1 << i; else s_ver &= ~((uint64_t)1 << i);
    if (s_nvs.begin("invprof", false)) { s_nvs.putULong64("ver", s_ver); s_nvs.end(); }
    return "";
}

static String jnum(float v, uint8_t dec) { return isnan(v) ? String("null") : String(v, (unsigned)dec); }
static String jstr(const char* s) { String r = s ? s : ""; r.replace("\"", "'"); return "\"" + r + "\""; }

String invSettingsJson() {
    setInit();
    String j;
    j.reserve(9000);
    float bt = setVal("bat"), mod = setVal("mod");
    j = "{\"bt\":" + jnum(bt, 0) + ",\"mod\":" + jnum(mod, 0) + ",\"busy\":" + (gBusy() || wBusy() ? "true" : "false");
    j += ",\"panelOnly\":" + jstr(PANEL_ONLY) + ",\"list\":[";
    for (uint8_t i = 0; i < NSETS; i++) {
        const SetDef& d = SETS[i];
        float lo, hi;
        setLimits(i, lo, hi);
        String why = setWhyNot(i);
        if (i) j += ",";
        j += "{\"id\":" + jstr(d.id) + ",\"p\":" + jstr(d.panel) + ",\"n\":" + String(d.prog) +
             ",\"g\":" + jstr(d.group) + ",\"t\":" + jstr(d.title) + ",\"k\":" + String(d.kind) +
             ",\"u\":" + jstr(d.unit) + ",\"d\":" + String(d.dec) + ",\"lo\":" + String(lo, (unsigned)d.dec) +
             ",\"hi\":" + String(hi, (unsigned)d.dec) + ",\"st\":" + String(d.step, (unsigned)d.dec) + ",\"l\":" + jstr(d.list) +
             ",\"o\":" + jstr(d.opts) + ",\"def\":" + String(d.def, (unsigned)d.dec) + ",\"v\":" + jnum(s_sv[i], (unsigned)d.dec) +
             ",\"s\":" + String(s_sup[i]) + ",\"ver\":" + ((s_ver >> i & 1) ? "true" : "false") +
             ",\"ro\":" + (d.rule == SR_READONLY || (!d.wr && d.kind != SK_FLAG) ? "true" : "false") +
             ",\"why\":" + jstr(why.c_str()) + ",\"note\":" + jstr(d.note) + "}";
    }
    return j + "]}";
}

String invSettingsText() {
    setInit();
    String r = "⚙️ Настройки инвертора\n";
    const char* g = "";
    for (uint8_t i = 0; i < NSETS; i++) {
        const SetDef& d = SETS[i];
        if (strcmp(g, d.group)) { g = d.group; r += String("\n") + g + ":\n"; }
        r += (d.prog ? (d.prog < 10 ? "0" : "") + String(d.prog) + " " : String("   ")) + (d.panel[0] ? String(d.panel) + " " : String("")) +
             d.title + ": " + (s_sup[i] == 0 ? String("не поддерживается") : fmtVal(i, s_sv[i])) + "\n";
    }
    return r;
}

uint32_t invSetSeq() { return s_wSeq; }
String   invSetLastMsg() { return s_wMsg; }

String invSetStatusJson() {
    static const char* const ST[] = { "idle", "busy", "busy", "busy", "busy", "done" };
    String m = s_wMsg;
    m.replace("\"", "'");
    return String("{\"state\":\"") + (gBusy() ? "busy" : ST[s_w]) + "\",\"ok\":" + (s_wOk ? "true" : "false") +
           ",\"seq\":" + String(s_wSeq) + ",\"msg\":\"" + m + "\"}";
}

size_t invBuildRequest(uint8_t, uint8_t* out, size_t cap) {
    // Если G-ASCII не отозвался за два круга — это не тот инвертор, переходим
    // на старый Q1, который у HS35 тоже есть.
    const char* c;
    char wcmd[8];
    // Переключение не ждёт ответа дольше 15 с.
    if ((s_w == W_SENT || s_w == W_VERIFY) && millis() - s_wMs > 15000) wFinish(false, "инвертор не ответил");
    if ((s_g == G_SENT || s_g == G_VERIFY) && millis() - s_gMs > 15000) gFinish(false, "инвертор не ответил");
    if (s_g == G_QUEUED) {
        fifoPush(K_WGEN);
        trace(String("→ ") + s_gCmd + " (запись)");
        s_g = G_SENT;
        s_gMs = millis();
        size_t n = strlen(s_gCmd);
        if (n + 1 > cap) return 0;
        memcpy(out, s_gCmd, n);
        out[n] = '\r';
        return n + 1;
    }
    if (s_g == G_READ) {                 // перечитать записанное
        const SetDef& d = SETS[s_gIdx];
        const char* rc = d.kind == SK_FLAG ? "TE?" : d.rd;
        fifoPush((Kind)(d.kind == SK_FLAG ? 0x80 + SET_TE : 0x80 + s_gIdx));
        trace(String("→ ") + rc + " (проверка записи)");
        s_g = G_VERIFY;
        s_gMs = millis();
        size_t n = strlen(rc);
        memcpy(out, rc, n);
        out[n] = '\r';
        return n + 1;
    }
    if (s_w == W_QUEUED) {
        snprintf(wcmd, sizeof(wcmd), "%s%02u", s_wWhat ? "CPR" : "OPR", s_wCode);
        fifoPush(s_wWhat ? K_WCPR : K_WOPR);
        trace(String("→ ") + wcmd + " (запись)");
        s_w = W_SENT;
        s_wMs = millis();
        size_t n = strlen(wcmd);
        memcpy(out, wcmd, n);
        out[n] = '\r';
        return n + 1;
    }
    uint8_t setKind = 0;
    if (s_w == W_READ) {                 // перечитать то, что записали
        c = NAME[s_wWhat ? K_CPR : K_OPR];
        s_w = W_VERIFY;
        s_wMs = millis();
    } else if (!s_gSeen && s_sent > 2 * sizeof(CYCLE)) {
        c = "Q1";
    } else if (s_gSeen && ++s_setTick >= 4 && (c = nextSetRead(setKind)) != nullptr) {
        s_setTick = 0;                   // каждая четвёртая — настройка из таблицы
    } else {
        if (s_setTick >= 4) s_setTick = 0;
        setKind = 0;
        c = NAME[CYCLE[s_pos]];
        s_pos = (s_pos + 1) % sizeof(CYCLE);
    }
    if (s_sent < 255) s_sent++;
    if (setKind) fifoPush((Kind)setKind);
    else for (uint8_t k = 0; k < K_COUNT; k++) if (!strcmp(NAME[k], c)) { fifoPush((Kind)k); break; }
    trace(String("→ ") + c);
    size_t n = strlen(c);
    if (n + 1 > cap) return 0;
    memcpy(out, c, n);
    out[n] = '\r';                       // без контрольной суммы: G-ASCII её не знает
    return n + 1;
}

// ------------------------------------------------------------------ разбор

static int split(const char* s, const char* f[], int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        f[n++] = s;
        while (*s && *s != ' ') s++;
    }
    return n;
}
static float num(const char* f)  { return strtof(f, nullptr); }
static long  inum(const char* f) { return strtol(f, nullptr, 10); }
static bool  hasDot(const char* f) { for (; *f && *f != ' '; f++) if (*f == '.') return true; return false; }
static int   flen(const char* f) { int n = 0; while (f[n] && f[n] != ' ') n++; return n; }

// Отпечатки ответов — по живому HS35 20.10.2026 и тестам ha-eybond-local.
static Kind classify(const char* line, const char* f[], int n) {
    if (!strncmp(line, "NAK", 3) || !strncmp(line, "(NAK", 4)) return K_NAK;
    if (!strncmp(line, "ACK", 3) || !strncmp(line, "(ACK", 4)) return K_ACK;
    if (!strncmp(line, "BL", 2))  return K_BL;
    if (line[0] == '#')           return n >= 4 ? K_F : K_UNKNOWN;      // #220.0 026 048.0 50.0
    if (line[0] != '(')           return K_UNKNOWN;
    if (n == 1 && flen(f[0]) == 1 && isalpha((uint8_t)f[0][0])) return K_GMOD;   // (B
    if (n == 1 && isdigit((uint8_t)f[0][0]) && flen(f[0]) <= 2) return K_SHORT;   // (00, (2 — настройки
    if (n == 2 && f[1][0] == '(') return K_SVFW;                        // (4.020 (20241011
    if (n == 3 && flen(f[1]) == 16) return K_GWS;                       // (00 0000… 0000…
    if (n == 8 && flen(f[7]) == 8) return K_Q1;
    if (n == 5) return hasDot(f[2]) ? K_GTMP : K_GBAT;                  // GBAT: 3-е поле — число элементов
    if (n == 13) return K_GLINE;
    if (n == 15) return K_GOP;
    if (n == 17) return K_GBMS;
    if (n == 22) return K_GPDAT0;
    if (n == 24) return K_GPV;
    if (n >= 20) return K_GCHG;
    return K_UNKNOWN;
}

static void fresh(InvState& v) { v.linked = true; v.fresh = true; v.lastFrameMs = millis(); }


// ------------------------------------------------------------------ заряд

// Процент приходит из трёх мест разной точности: GBMS — от BMS самой
// батареи, с десятыми; BL — уровень по версии инвертора; GPDAT0 поле 17 —
// грубая оценка инвертора по напряжению, крупным шагом. Показывался тот,
// что пришёл последним, и цифра скакала: на экране то 38, то 9, то 100.
// Теперь менее точный источник не перебивает более точный, пока тот
// приходит; замолчал на 2 минуты — берём следующий.
enum SocSrc : uint8_t { SOC_NONE, SOC_GPDAT0, SOC_BL, SOC_GBMS };
static const char* const SOC_NAME[] = { "—", "GPDAT0 (оценка по напряжению)", "BL", "GBMS (BMS батареи)" };
static uint8_t  s_socSrc = SOC_NONE;
static uint32_t s_socMs = 0;
static float    s_socLast = -1;
static String   s_jumps;                 // скачки больше 15 пунктов — для разбора

static void setSoc(float soc, SocSrc src, const char* line) {
    if (soc < 0 || soc > 100) return;
    // Оценка инвертора бывает нулём при живой батарее (21.10.2026: 0 % при
    // 49,8 В, сразу после перезагрузки модуля, пока BMS ещё не ответила) —
    // бот слал «батарея 0 %». Её ноль не берём никогда, а первые 3 минуты
    // после включения не берём её вовсе: ждём BMS (она в круге раз в ~75 с).
    if (src == SOC_GPDAT0 && (soc < 1 || millis() < 180000)) return;
    // Источник хуже текущего ждёт, пока лучший замолчит. BMS в круге
    // отвечает раз в ~75 с (круг ~37 команд по 2 с), и 2 минут ожидания не
    // хватало: 22.10.2026 при заряде от сети инвертор в GPDAT0 давал 9 %
    // при 44–51 % у BMS, и на дашборде мелькали 9 %. Теперь BMS ждём
    // 10 минут — оценка инвертора нужна, только если BMS правда пропала.
    uint32_t hold = s_socSrc == SOC_GBMS ? 600000 : 120000;
    if (src < s_socSrc && millis() - s_socMs < hold) return;
    if (s_socLast >= 0 && fabsf(soc - s_socLast) > 15 && s_jumps.length() < 900) {
        char b[48];
        snprintf(b, sizeof(b), "    %.0f%% → %.0f%% из %s: ", s_socLast, soc, SOC_NAME[src]);
        s_jumps += String(b) + String(line).substring(0, 100) + "\n";
        if (s_nvs.begin("invprof", false)) { s_nvs.putString("jumps", s_jumps); s_nvs.end(); }
    }
    s_socSrc = src;
    s_socMs = millis();
    s_socLast = soc;
    stateUpdate([&](BmsState& b) {
        b.soc = soc;
        b.socKnown = true;
        if (s_capAh > 0) { b.remainingMah = (uint32_t)(s_capAh * soc * 10.0f); b.capAh = s_capAh; }   // А·ч × % → мА·ч
    });
}
static void bmsFresh(BmsState& b) { b.linked = true; b.fresh = true; b.lastFrameMs = millis(); }

// Счётчик из двух полей: старшие и младшие 16 бит, в сотых кВт·ч. На HS35
// «всего» совпадает с «сегодня» — похоже, это суточный счётчик; статистика
// берёт только положительные приращения, так что сброс в полночь не мешает.
static double counter(const char* hi, const char* lo) {
    return (double)(((uint32_t)inum(hi) << 16) + (uint32_t)inum(lo)) / 100.0;
}

// GPDAT0: (0 5 4020 0 00 220.0 50.02 240.1 50.01 217.9 50.02 03.36 49.2 000.0
//          015 0739 0607 050 267.6 000.0 0784 060.0
// 1 режим, 7–8 сеть, 9–10 выход, 12 батарея В, 13 ток (без знака), 14 загрузка,
// 15 ВА, 16 Вт, 17 заряд %, 18 панели В, 20 панели Вт, 21 температура.
static void applyGpdat0(const char* f[], const char* line) {
    uint8_t mode;
    switch (f[1][0]) {
    case '2': mode = INVM_FAULT;   break;
    case '4': mode = INVM_LINE;    break;
    case '5': mode = INVM_BATTERY; break;
    default:  mode = INVM_STANDBY; break;
    }
    // Мощности из сети в протоколе нет — считаем по балансу: в режиме «от
    // сети» сеть кормит нагрузку и заряд батареи, минус то, что дают панели.
    // Раньше было «нагрузка + заряд от панелей», и ночной заряд от сети
    // (22.10.2026: 20 А = RCC, 1000 Вт) в «сети» не появлялся вовсе — на
    // схеме сеть 762 Вт уходила в нагрузку 762 Вт, а заряд 995 Вт брался
    // ниоткуда. Ток заряда — от BMS: он точный и со знаком.
    BmsState bms = stateSnapshot();
    float battInW = bms.fresh && bms.current > 0 ? bms.voltage * bms.current : 0;
    invUpdate([&](InvState& v) {
        fresh(v);
        v.mode    = mode;
        v.gridV   = num(f[7]);
        v.gridHz  = num(f[8]);
        v.outV    = num(f[9]);
        v.outHz   = num(f[10]);
        v.loadPct = (uint8_t)inum(f[14]);
        v.outW    = num(f[16]);
        v.pvV     = num(f[18]);
        v.pvW     = num(f[20]);
        v.tempC   = (int16_t)num(f[21]);
        v.gridOn  = v.gridV > 50.0f;
        // Энергию за день статистика всё равно берёт со счётчика GLINE;
        // это — мощность для схемы и графика.
        float g = v.outW + battInW - v.pvW;
        v.gridW   = mode == INVM_LINE && g > 0 ? g : 0;
    });
    stateUpdate([&](BmsState& b) {
        bmsFresh(b);
        b.voltage  = num(f[12]);
    });
    setSoc(inum(f[17]), SOC_GPDAT0, line);
}

// GPV: (260.4 049.1 00.00 02.95 00768 … 20 сегодня, 21–22 всего
static void applyGpv(const char* f[]) {
    invUpdate([&](InvState& v) {
        fresh(v);
        v.pvV  = num(f[0]);
        v.pvW  = num(f[4]);
        v.chgW = num(f[1]) * num(f[2]);   // заряд от панелей: батарея В × ток заряда
        v.pvKwhTotal = counter(f[21], f[22]);
        // «Сегодня» (поле 20, и так же у GOP и GLINE) не берём: 22.10.2026
        // оно давало солнце 1,73 кВт·ч при 10,9 по приращению «всего» и 11,4
        // по мощности панелей; сеть и нагрузка — тоже в разы меньше. Похоже,
        // у инвертора свои часы, и его сутки начинаются не в полночь.
        // «За сегодня» считает статистика платы по счётчикам «всего».
    });
}

// GOP: (220.0 50.02 003.87 00.00 00678 00000 00851 … 014 … 12 сегодня, 13–14 всего
static void applyGop(const char* f[]) {
    invUpdate([&](InvState& v) {
        fresh(v);
        v.outV    = num(f[0]);
        v.outHz   = num(f[1]);
        v.outW    = num(f[4]);
        v.loadPct = (uint8_t)inum(f[9]);
        v.loadKwhTotal = counter(f[13], f[14]);
    });
}

// GLINE: (240.4 50.02 264.0 154.0 255.0 163.0 70.00 40.00 013 00000 00759 00000 00759
static void applyGline(const char* f[]) {
    invUpdate([&](InvState& v) {
        fresh(v);
        v.gridV  = num(f[0]);
        v.gridHz = num(f[1]);
        v.gridOn = v.gridV > 50.0f;
        v.gridKwhTotal = counter(f[11], f[12]);
    });
}

// GBMS: (00008 00000 00491 65446 00220 00450 32767 02000 …
// 2 напряжение /10, 3 ток /100 со знаком, 4 температура /10, 5 заряд /10,
// 6 остаток /10, 7 ёмкость /10, 8 код аварии BMS, 9 код предупреждения,
// 10 разрешённый ток заряда /100, 11 напряжение заряда /10 (поля — по
// ha-eybond-local). 32767 и 65535 — «нет данных».
static void applyGbms(const char* f[], const char* line) {
    long rawI = inum(f[3]);
    if (rawI == 65535 || inum(f[2]) == 65535) return;    // BMS не на связи
    float amps = (int16_t)(uint16_t)rawI / 100.0f;
    long cap = inum(f[7]);
    if (cap > 0 && cap != 32767 && cap != 65535) s_capAh = cap / 10.0f;
    long soc10 = inum(f[5]);
    stateUpdate([&](BmsState& b) {
        bmsFresh(b);
        b.voltage = inum(f[2]) / 10.0f;
        b.current = amps;                 // минус — разряд
        b.chargeState = amps > 0.5f ? 1 : (amps < -0.5f ? 2 : 0);
        b.tempMax = b.tempMin = (int16_t)(inum(f[4]) / 10);
        long lim = inum(f[10]), cv = inum(f[11]);
        b.chgLimitA = (lim >= 0 && lim < 32767) ? lim / 100.0f : -1;
        b.cvV       = (cv > 0 && cv < 32767) ? cv / 10.0f : -1;
        long fc = inum(f[8]), wc = inum(f[9]);
        b.bmsFault  = (fc > 0 && fc < 32767) ? (uint16_t)fc : 0;
        b.bmsWarn   = (wc > 0 && wc < 32767) ? (uint16_t)wc : 0;
    });
    if (soc10 >= 0 && soc10 <= 1000) setSoc(soc10 / 10.0f, SOC_GBMS, line);
}

static void applyGws(const char* f[]) {
    long code = inum(f[0]);
    invUpdate([&](InvState& v) {
        fresh(v);
        v.faultCode = (uint8_t)code;
        // 1–29 — авария, инвертор в режиме неисправности; 50–70 — только
        // предупреждение, он работает дальше (руководство, 3.5.1–3.5.2).
        if (code && code < 50) v.mode = INVM_FAULT;
    });
}

static void applyGmod(const char* f) {
    uint8_t m;
    switch (f[0]) {
    case 'L': m = INVM_LINE;    break;
    case 'B': m = INVM_BATTERY; break;
    case 'F': m = INVM_FAULT;   break;
    default:  m = INVM_STANDBY; break;
    }
    invUpdate([&](InvState& v) { fresh(v); v.mode = m; });
}

// Q1 — запасной путь, если G-ASCII нет: (228.9 000.0 228.8 013 50.0 2.06 58.0 00100000
static void applyQ1(const char* f[]) {
    const char* bits = f[7];
    bool gridFail = bits[0] == '1', battLow = bits[1] == '1';
    bool bypass = bits[2] == '1', failed = bits[3] == '1';
    float cellV = num(f[5]);
    float battV = cellV < 5.0f ? cellV * (s_ratedBattV / 2.0f) : cellV;
    invUpdate([&](InvState& v) {
        fresh(v);
        v.gridV = num(f[0]); v.outV = num(f[2]);
        v.loadPct = (uint8_t)inum(f[3]);
        v.gridHz = v.outHz = num(f[4]);
        v.tempC = (int16_t)num(f[6]);
        v.outW = s_ratedVA * v.loadPct / 100.0f;        // оценка, ВА
        v.gridOn = !gridFail && v.gridV > 50.0f;
        v.gridW = (v.gridOn && bypass) ? v.outW : 0;
        v.faultCode = failed ? 1 : (battLow ? 2 : 0);
        if (failed)        v.mode = INVM_FAULT;
        else if (gridFail) v.mode = INVM_BATTERY;
        else if (bypass)   v.mode = INVM_BYPASS;
        else if (v.gridOn) v.mode = INVM_LINE;
        else               v.mode = INVM_STANDBY;
    });
    stateUpdate([&](BmsState& b) { bmsFresh(b); b.voltage = battV; });
}

// «#220.0 026 048.0 50.0»: напряжение, ток, батарея, частота — если похоже.
static void applyF(const char* f[]) {
    float V = num(f[0] + 1), A = num(f[1]), B = num(f[2]);
    if (V > 100 && V < 260 && A > 1 && A < 100 && B >= 12 && B <= 60) { s_ratedVA = V * A; s_ratedBattV = B; }
}

static void applySetting(Kind k, long code) {
    invUpdate([&](InvState& v) {
        if (k == K_OPR && code >= 0 && code <= 2) v.prioOut = code;
        // CHP у HS35: 0 PV, 1 PNG, 2 OPV — сверено переключением с панели 21.10.2026.
        if (k == K_CPR && code >= 0 && code <= 3) v.prioChg = code;
        if (k == K_TBAT && code >= 0 && code <= 3) v.battType = code;
    });
}

static void remember(Kind k, const char* line) {
    if (k >= K_COUNT || s_seen[k]) return;
    s_seen[k] = true;
    s_report += String("    ") + NAME[k] + " → " + String(line).substring(0, 110) + "\n";
    if (s_nvs.begin("invprof", false)) { s_nvs.putString("disc", s_report); s_nvs.end(); }
}

bool invFeed(uint8_t, const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c != '\r') {
            if (s_len < sizeof(s_buf) - 1) s_buf[s_len++] = c;
            continue;
        }
        s_buf[s_len] = 0;
        s_len = 0;
        const char* line = s_buf;
        // Пустые строки (бывают между ответами) — не ответ: очередь не трогаем,
        // иначе снимется команда, ответ на которую ещё впереди.
        if (!line[0]) continue;
        const char* body = line[0] == '(' ? line + 1 : line;
        const char* f[32];
        int nf = split(body, f, 32);
        Kind k = classify(line, f, nf);
        {
            String t = String("← ") + String(line).substring(0, 40);
            if (k == K_SHORT || k == K_NAK || k == K_ACK || k == K_UNKNOWN)
                t += s_fLen ? String(" [голова: ") + kname(s_fifo[s_fHead]) + "]" : String(" [очередь пуста]");
            else t += String(" [") + kname(k) + "]";
            trace(t);
        }
        // Голова очереди — чтение настройки из таблицы: короткий ответ любого
        // вида («(56.4», «(030», «(SIQ», пустой «(») — её, NAK — тоже её.
        // V??? отвечает двумя полями — «(220 220»: берётся первое. Два поля со
        // скобкой во втором — это SVFW, не настройка.
        {
            uint8_t head = s_fLen ? (uint8_t)s_fifo[s_fHead] : 0xFF;
            bool setHead = head >= 0x80 && head < K_WOPR;
            bool short1 = nf <= 1 || (nf == 2 && f[1][0] != '(');
            if (setHead && line[0] == '(' && short1 && k != K_ACK && k != K_NAK) {
                fifoPop();
                applySetValue(head - 0x80, nf ? f[0] : "");
                continue;
            }
            if (setHead && k == K_NAK) {
                fifoPop();
                setNak(head - 0x80);
                continue;
            }
        }
        if (k == K_SHORT) {
            Kind head = fifoPop();
            if (head == K_OPR || head == K_CPR || head == K_OPM || head == K_TBAT) {
                remember(head, line);
                applySetting(head, inum(f[0]));
                // Перечитали записанное — сверяем.
                if (head == K_OPM) { setInit(); int8_t m = setIdx("mod"); s_sv[m] = inum(f[0]); s_sup[m] = 1; }
                if (head == K_TBAT) { setInit(); int8_t b = setIdx("bat"); s_sv[b] = inum(f[0]); s_sup[b] = 1; }
                if (s_w == W_VERIFY && head == (s_wWhat ? K_CPR : K_OPR)) {
                    long got = inum(f[0]);
                    if (got == s_wCode) wFinish(true, "встало");
                    else wFinish(false, "инвертор ответил ACK, но значение осталось " + String(got));
                }
            }
            continue;
        }
        if (k == K_ACK || k == K_NAK) {
            // ACK обычные команды не дают никогда — значит, это ответ на запись,
            // даже если перед ней потерялся чей-то ответ: выравниваемся до неё.
            // NAK бывает и на обычные — его приписываем записи, только если
            // запись в голове очереди.
            Kind head;
            if (k == K_ACK) {
                head = K_UNKNOWN;
                while (s_fLen) { head = fifoPop(); if (head == K_WOPR || head == K_WCPR || head == K_WGEN) break; }
            } else {
                head = fifoPop();
            }
            if ((head == K_WOPR || head == K_WCPR) && s_w == W_SENT) {
                if (k == K_ACK) { s_w = W_READ; s_wMs = millis(); }
                else wFinish(false, "инвертор отказал (NAK)");
            }
            if (head == K_WGEN && s_g == G_SENT) {
                if (k == K_ACK) { s_g = G_READ; s_gMs = millis(); }
                else gFinish(false, "инвертор отказал (NAK) — команда записи у HS35, похоже, другая");
            }
            continue;
        }
        if (k == K_UNKNOWN) { fifoPop(); continue; }
        fifoSync(k);
        if (k != K_Q1) { s_gSeen = true; s_sent = 0; }
        remember(k, line);
        switch (k) {
        case K_GPDAT0: applyGpdat0(f, line); break;
        case K_GPV:    applyGpv(f);    break;
        case K_GOP:    applyGop(f);    break;
        case K_GLINE:  applyGline(f);  break;
        case K_GBMS:   applyGbms(f, line); break;
        case K_BL: {
            long soc = inum(line + 2);
            stateUpdate([&](BmsState& b) { bmsFresh(b); });
            setSoc(soc, SOC_BL, line);
            break;
        }
        case K_GWS:    applyGws(f);    break;
        case K_GMOD:   applyGmod(f[0]); break;
        case K_Q1:     applyQ1(f);     break;
        case K_F:      applyF(f);      break;
        default: break;                  // GBAT, GTMP, GCHG, SVFW — только в отчёте
        }
    }
    return true;                         // шаг ведёт сам профиль
}

String invTrace() {
    String r;
    for (uint8_t i = 0; i < s_tLen; i++) r += s_trace[(s_tHead + i) % 40] + "\n";
    return r;
}

String invProfileReport() {
    String r = s_report, j = s_jumps;
    if ((!r.length() || !j.length()) && s_nvs.begin("invprof", true)) {
        if (!r.length()) r = s_nvs.getString("disc", "");
        if (!j.length()) j = s_nvs.getString("jumps", "");
        s_nvs.end();
    }
    r += String("    заряд сейчас из: ") + SOC_NAME[s_socSrc] + "\n";
    if (j.length()) r += "    скачки заряда:\n" + j;
    return r;
}

#endif
