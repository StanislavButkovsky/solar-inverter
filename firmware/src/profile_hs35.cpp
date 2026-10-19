// Профиль второго инвертора: Hiden Control HS35-5648PRO по USB.
//
// Ждали Modbus, как у MUST PV18, а разведки 19.10.2026 показали: за USB мост
// CH340, порт на 9600 8N1 разбирает строки до \r, отвечает голым «NAK\r»
// на незнакомое и принимает команду только побайтно с паузой от 5 мс.
// Из PI30, PI18, PI17 и Megatec ответили лишь Q1 (состояние, как у ИБП), I и F.
//
// Ровно такое поведение у протокола JsdSolar «G-command ASCII», который
// говорят инверторы JSD через датлоггеры EyeBond (Wi-Fi-модуль Hiden — это
// EyeBond с приложением SmartClient). Описан в groove-max/ha-eybond-local:
// команды GPDAT0, GPV, GBAT, GOP, GLINE, BL и другие, с мощностью панелей,
// током и зарядом батареи и собственными счётчиками кВт·ч. Наша батарея
// проверок их ни разу не спрашивала.
//
// Поэтому профиль начинает с опроса возможностей: спрашивает каждую команду
// G-ASCII, запоминает, какие ответили, и пишет их живые ответы в NVS — чтобы
// дома сверить раскладку полей. Дальше опрашивает ответившими, а если их нет —
// старым Q1.
#if defined(TARGET_HS35) && !defined(TARGET_ASTERION) && !defined(TARGET_MUST)

#include "profile.h"
#include "state.h"
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

const char*    INV_PROFILE_NAME = "JsdSolar G-ASCII / Megatec Q1 (HS35 по USB)";
const uint32_t INV_BAUD   = 9600;
const uint16_t INV_GAP_MS = 10;          // ответ с 5 мс, берём с запасом

// Шагами управляет сам профиль: inv.cpp лишь просит запрос и отдаёт байты.
const uint8_t INV_NSTEPS = 1;

// ------------------------------------------------------------------ команды

enum Cmd : uint8_t {
    C_GPDAT0, C_GPV, C_GOP, C_GLINE, C_GBAT, C_BL, C_GMOD, C_GWS, C_GTMP, C_GBMS,
    C_GCHG, C_F, C_SVFW, C_Q1, C_COUNT
};
static const char* const CMD[C_COUNT] = {
    "GPDAT0", "GPV", "GOP", "GLINE", "GBAT", "BL", "GMOD", "GWS", "GTMP", "GBMS",
    "GCHG", "F", "SVFW", "Q1",
};

// Ответила ли команда при опросе возможностей.
static bool s_has[C_COUNT];
static bool s_discovered = false;
static uint8_t s_probe = 0;              // какую команду опроса возможностей спрашиваем

// Круг опроса, когда G-ASCII есть: живые данные через шаг, остальное по кругу.
static const Cmd G_CYCLE[] = {
    C_GPDAT0, C_GPV, C_GPDAT0, C_GOP, C_GPDAT0, C_GLINE, C_GPDAT0, C_BL,
    C_GPDAT0, C_GBAT, C_GPDAT0, C_GWS, C_GPDAT0, C_GMOD,
};
static uint8_t s_pos = 0;

static Cmd    s_cur = C_GPDAT0;          // что спросили последним
static uint8_t s_tries = 0;              // сколько раз подряд без ответа
static char   s_buf[200];
static size_t s_len = 0;

// Отчёт опроса возможностей — в NVS, для status дома.
static Preferences s_nvs;
static String s_report;

// Паспорт HS35-5648PRO: 5600 ВА, батарея 48 В. Ответ F их уточняет.
static float s_ratedVA = 5600, s_ratedBattV = 48;

void invResetStream() { s_len = 0; }

static bool useG() { return s_has[C_GPDAT0] || s_has[C_GPV] || s_has[C_GOP]; }

static Cmd nextCmd() {
    if (!s_discovered) return (Cmd)s_probe;
    if (!useG()) return C_Q1;
    for (uint8_t i = 0; i < sizeof(G_CYCLE); i++) {
        Cmd c = G_CYCLE[s_pos];
        s_pos = (s_pos + 1) % sizeof(G_CYCLE);
        if (s_has[c]) return c;
    }
    return C_Q1;
}

static void noteProbe(Cmd c, const char* reply) {
    String line = String(CMD[c]) + " → " + (reply ? String(reply).substring(0, 110) : String("тишина"));
    s_report += "    " + line + "\n";
}

static void probeAdvance() {
    if (++s_probe >= C_COUNT) {
        s_discovered = true;
        int n = 0;
        for (int i = 0; i < C_COUNT; i++) n += s_has[i];
        s_report += String("    ответили ") + n + " из " + C_COUNT + ", опрос: " + (useG() ? "G-ASCII" : "Q1") + "\n";
        if (!s_nvs.begin("invprof", false)) return;
        s_nvs.putString("disc", s_report);
        s_nvs.end();
        Serial.printf("[инв] возможности опрошены, опрос: %s\n", useG() ? "G-ASCII" : "Q1");
    }
}

size_t invBuildRequest(uint8_t, uint8_t* out, size_t cap) {
    // Прошлый запрос остался без ответа: в опросе возможностей — две попытки
    // и дальше, в работе — просто спрашиваем следующее.
    if (s_tries && !s_discovered && s_tries >= 2) {
        noteProbe((Cmd)s_probe, nullptr);
        probeAdvance();
        s_tries = 0;
    }
    s_cur = (s_tries && !s_discovered) ? s_cur : nextCmd();
    s_tries++;
    const char* c = CMD[s_cur];
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
static float num(const char* f) { return strtof(f, nullptr); }
static long  inum(const char* f) { return strtol(f, nullptr, 10); }

// Счётчик из двух полей: старшие и младшие 16 бит, в сотых кВт·ч.
static double counter(const char* hi, const char* lo) {
    return (double)(((uint32_t)inum(hi) << 16) + (uint32_t)inum(lo)) / 100.0;
}

static uint8_t gpdatMode(char c) {
    switch (c) {
    case '2': return INVM_FAULT;
    case '3': return INVM_STANDBY;
    case '4': return INVM_LINE;
    case '5': return INVM_BATTERY;
    default:  return INVM_STANDBY;     // включение, выключение, тест
    }
}

static void fresh(InvState& v) { v.linked = true; v.fresh = true; v.lastFrameMs = millis(); }

// Направление тока батареи протокол знаком не помечает. Пока раскладку не
// сверили с живым инвертором: разряд — когда работаем от батареи и солнце не
// покрывает нагрузку, иначе заряд.
static float battSign(const InvState& v) {
    return (v.mode == INVM_BATTERY && v.pvW < v.outW) ? -1.0f : 1.0f;
}

static void applyGpdat0(const char* f[], int n) {
    if (n < 22) return;
    float battA = num(f[13]);
    uint8_t soc = (uint8_t)inum(f[17]);
    InvState snap;
    invUpdate([&](InvState& v) {
        fresh(v);
        v.mode    = gpdatMode(f[1][0]);
        v.gridV   = num(f[7]);
        v.gridHz  = num(f[8]);
        v.outV    = num(f[9]);
        v.outHz   = num(f[10]);
        v.loadPct = (uint8_t)inum(f[14]);
        v.outW    = num(f[16]);           // активная мощность, Вт
        v.pvV     = num(f[18]);
        v.pvW     = num(f[20]);
        v.tempC   = (int16_t)num(f[21]);
        v.gridOn  = v.gridV > 50.0f;
        v.gridW   = v.mode == INVM_LINE ? v.outW : 0;   // уточнится счётчиком GLINE
        if (v.mode == INVM_FAULT) v.faultCode = v.faultCode ? v.faultCode : 1;
        snap = v;
    });
    stateUpdate([&](BmsState& b) {
        b.linked = true; b.fresh = true; b.lastFrameMs = millis();
        b.voltage  = num(f[12]);
        b.current  = battA * battSign(snap);
        b.soc      = soc;
        b.socKnown = true;
    });
}

static void applyGpv(const char* f[], int n) {
    if (n < 5) return;
    invUpdate([&](InvState& v) {
        fresh(v);
        v.pvV = num(f[0]);
        v.pvW = num(f[4]);
        v.chgW = num(f[1]) * num(f[2]);   // заряд от панелей: напряжение батареи × ток заряда
        if (n >= 23) v.pvKwhTotal = counter(f[21], f[22]);
    });
}

static void applyGop(const char* f[], int n) {
    if (n < 10) return;
    invUpdate([&](InvState& v) {
        fresh(v);
        v.outV    = num(f[0]);
        v.outHz   = num(f[1]);
        v.outW    = num(f[4]);
        v.loadPct = (uint8_t)inum(f[9]);
        if (n >= 15) v.loadKwhTotal = counter(f[13], f[14]);
    });
}

static void applyGline(const char* f[], int n) {
    if (n < 2) return;
    invUpdate([&](InvState& v) {
        fresh(v);
        v.gridV  = num(f[0]);
        v.gridHz = num(f[1]);
        v.gridOn = v.gridV > 50.0f;
        if (n >= 13) v.gridKwhTotal = counter(f[11], f[12]);
    });
}

static void applyGbat(const char* f[], int n) {
    if (n < 2) return;
    InvState v = invSnapshot();
    stateUpdate([&](BmsState& b) {
        b.linked = true; b.fresh = true; b.lastFrameMs = millis();
        b.voltage = num(f[0]);
        b.current = num(f[1]) * battSign(v);
    });
}

static void applyGws(const char* f[], int n) {
    if (n < 1) return;
    long code = inum(f[0]);
    invUpdate([&](InvState& v) {
        fresh(v);
        v.faultCode = (uint8_t)code;
        if (code) v.mode = INVM_FAULT;
    });
}

static void applyGmod(const char* s) {
    uint8_t m;
    switch (s[0]) {
    case 'L': m = INVM_LINE;    break;
    case 'B': m = INVM_BATTERY; break;
    case 'F': m = INVM_FAULT;   break;
    default:  m = INVM_STANDBY; break;
    }
    invUpdate([&](InvState& v) { fresh(v); v.mode = m; });
}

// Q1: (228.9 000.0 228.8 013 50.0 2.06 58.0 00100000 — запасной путь,
// если G-ASCII нет. Мощность — оценка через номинал, процента заряда нет.
static void applyQ1(const char* f[], int n) {
    if (n < 8 || strlen(f[7]) < 8) return;
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
        v.outW = s_ratedVA * v.loadPct / 100.0f;
        v.gridOn = !gridFail && v.gridV > 50.0f;
        v.gridW = (v.gridOn && bypass) ? v.outW : 0;
        v.faultCode = failed ? 1 : (battLow ? 2 : 0);
        if (failed)        v.mode = INVM_FAULT;
        else if (gridFail) v.mode = INVM_BATTERY;
        else if (bypass)   v.mode = INVM_BYPASS;
        else if (v.gridOn) v.mode = INVM_LINE;
        else               v.mode = INVM_STANDBY;
    });
    stateUpdate([&](BmsState& b) {
        b.linked = true; b.fresh = true; b.lastFrameMs = millis();
        b.voltage = battV;
        b.socKnown = false;
    });
}

// «#220.0 016 024.0 50.0»: напряжение, ток, батарея, частота — если похоже.
static void applyF(const char* f[], int n) {
    if (n < 4) return;
    float V = num(f[0] + (f[0][0] == '#')), A = num(f[1]), B = num(f[2]);
    if (V > 100 && V < 260 && A > 1 && A < 100 && B >= 12 && B <= 60) { s_ratedVA = V * A; s_ratedBattV = B; }
}

bool invFeed(uint8_t, const uint8_t* data, size_t n) {
    if (s_len + n >= sizeof(s_buf)) s_len = 0;
    memcpy(s_buf + s_len, data, n);
    s_len += n;
    if (!s_len || s_buf[s_len - 1] != '\r') return false;   // строка ещё не кончилась
    s_buf[--s_len] = 0;
    s_len = 0;
    s_tries = 0;

    const char* line = s_buf;
    bool nak = !strncmp(line, "NAK", 3);

    if (!s_discovered) {
        s_has[s_cur] = !nak && line[0];
        noteProbe(s_cur, line);
        probeAdvance();
    }
    if (nak) return true;                // в работе — просто следующий шаг

    // Тело без вводного '(' — поля через пробел.
    const char* body = line[0] == '(' ? line + 1 : line;
    const char* f[32];
    int nf = split(body, f, 32);
    switch (s_cur) {
    case C_GPDAT0: applyGpdat0(f, nf); break;
    case C_GPV:    applyGpv(f, nf);    break;
    case C_GOP:    applyGop(f, nf);    break;
    case C_GLINE:  applyGline(f, nf);  break;
    case C_GBAT:   applyGbat(f, nf);   break;
    case C_BL:
        if (!strncmp(line, "BL", 2)) {
            long soc = inum(line + 2);
            stateUpdate([&](BmsState& b) { b.soc = soc; b.socKnown = true; b.lastFrameMs = millis(); });
        }
        break;
    case C_GWS:    applyGws(f, nf);    break;
    case C_GMOD:   applyGmod(body);    break;
    case C_Q1:     applyQ1(f, nf);     break;
    case C_F:      applyF(f, nf);      break;
    default: break;                      // GTMP, GBMS, GCHG, SVFW — пока только в отчёте
    }
    return true;
}

String invProfileReport() {
    if (s_discovered) return s_report;
    String r;
    if (s_nvs.begin("invprof", true)) { r = s_nvs.getString("disc", ""); s_nvs.end(); }
    return r.length() ? "  (с прошлого подключения)\n" + r : r;
}

#endif
