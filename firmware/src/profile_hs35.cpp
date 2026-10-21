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
    K_OPR, K_CPR, K_OPM, K_TBAT,                 // настройки: только чтение
    K_COUNT, K_SHORT = 0xFD, K_NAK = 0xFE, K_UNKNOWN = 0xFF
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

size_t invBuildRequest(uint8_t, uint8_t* out, size_t cap) {
    // Если G-ASCII не отозвался за два круга — это не тот инвертор, переходим
    // на старый Q1, который у HS35 тоже есть.
    const char* c;
    if (!s_gSeen && s_sent > 2 * sizeof(CYCLE)) {
        c = "Q1";
    } else {
        c = NAME[CYCLE[s_pos]];
        s_pos = (s_pos + 1) % sizeof(CYCLE);
    }
    if (s_sent < 255) s_sent++;
    for (uint8_t k = 0; k < K_COUNT; k++) if (!strcmp(NAME[k], c)) { fifoPush((Kind)k); break; }
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
    if (!strncmp(line, "NAK", 3)) return K_NAK;
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
    if (src < s_socSrc && millis() - s_socMs < 120000) return;
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
        // Мощности из сети в протоколе нет. В режиме «от сети» нагрузку и
        // заряд кормит сеть; энергию статистика всё равно берёт со счётчика GLINE.
        v.gridW   = mode == INVM_LINE ? v.outW + v.chgW : 0;
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
        v.pvKwhToday = inum(f[20]) / 100.0f;
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
        v.loadKwhToday = inum(f[12]) / 100.0f;
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
        v.gridKwhToday = inum(f[10]) / 100.0f;
    });
}

// GBMS: (00008 00000 00491 65446 00220 00450 32767 02000 …
// 2 напряжение /10, 3 ток /100 со знаком, 4 температура /10, 5 заряд /10,
// 6 остаток /10, 7 ёмкость /10. 32767 и 65535 — «нет данных».
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
    });
    if (soc10 >= 0 && soc10 <= 1000) setSoc(soc10 / 10.0f, SOC_GBMS, line);
}

static void applyGws(const char* f[]) {
    long code = inum(f[0]);
    invUpdate([&](InvState& v) {
        fresh(v);
        v.faultCode = (uint8_t)code;
        if (code) v.mode = INVM_FAULT;
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
            if (k == K_SHORT || k == K_NAK || k == K_UNKNOWN) t += s_fLen ? String(" [голова: ") + NAME[s_fifo[s_fHead]] + "]" : String(" [очередь пуста]");
            else t += String(" [") + NAME[k] + "]";
            trace(t);
        }
        if (k == K_SHORT) {
            Kind head = fifoPop();
            if (head == K_OPR || head == K_CPR || head == K_OPM || head == K_TBAT) {
                remember(head, line);
                applySetting(head, inum(f[0]));
            }
            continue;
        }
        if (k == K_NAK || k == K_UNKNOWN) { fifoPop(); continue; }
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
