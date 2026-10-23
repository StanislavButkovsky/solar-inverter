// BMS PACE по RS485. Описание и источники — в pace.h.
//
// Флаги всех трёх протоколов сводятся к разметке Modbus (руководство PACE,
// описания 1–3), в ASCII 0x25 они лежат в том же порядке:
//   warn   0 ячейка выше, 1 ячейка ниже, 2 батарея выше, 3 батарея ниже,
//          4 ток заряда, 5 ток разряда, 8–11 температура заряда/разряда
//          высокая/низкая, 12–13 среда высокая/низкая, 14 ключи горячие,
//          15 мало заряда;
//   prot   0–5 как у warn, 6 КЗ, 7 перенапряжение зарядного, 8–11 как у
//          warn, 12 ключи, 13–14 среда, 15 «полностью» (ASCII);
//   fault  0 ключ заряда, 1 ключ разряда, 2 датчик температуры, 3 связь,
//          4 ячейка, 5 измерение.
#include "pace.h"
#include <math.h>
#include <string.h>

// ------------------------------------------------------------------ ASCII

static const char HEX_[] = "0123456789ABCDEF";
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool hb(const char* s, size_t n, size_t& off, uint8_t& v) {
    if (off + 2 > n) return false;
    int a = hexv(s[off]), b = hexv(s[off + 1]);
    if (a < 0 || b < 0) return false;
    v = a << 4 | b; off += 2;
    return true;
}
static bool hw(const char* s, size_t n, size_t& off, uint16_t& v) {
    uint8_t a, b;
    if (!hb(s, n, off, a) || !hb(s, n, off, b)) return false;
    v = a << 8 | b;
    return true;
}
static void put2(char* o, size_t& i, uint8_t v) { o[i++] = HEX_[v >> 4]; o[i++] = HEX_[v & 15]; }

static uint16_t ckLen(uint16_t len) {
    uint16_t s = (len & 0xF) + ((len >> 4) & 0xF) + ((len >> 8) & 0xF);
    s = (~s + 1) & 0xF;
    return s << 12 | (len & 0x0FFF);
}
static uint16_t ckSum(const char* f, size_t from, size_t to) {   // [from, to)
    uint32_t s = 0;
    for (size_t i = from; i < to; i++) s += (uint8_t)f[i];
    return (~s + 1) & 0xFFFF;
}

size_t paceAsciiRequest(char* o, size_t cap, uint8_t ver, uint8_t addr, uint8_t cid2) {
    if (cap < 21) return 0;
    size_t i = 0;
    o[i++] = '~';
    put2(o, i, ver); put2(o, i, addr); put2(o, i, 0x46); put2(o, i, cid2);
    uint16_t l = ckLen(2);                                   // нагрузка — адрес, два знака
    put2(o, i, l >> 8); put2(o, i, l & 0xFF);
    put2(o, i, addr);
    uint16_t c = ckSum(o, 1, i);
    put2(o, i, c >> 8); put2(o, i, c & 0xFF);
    o[i++] = '\r';
    o[i] = 0;
    return i;
}

bool paceAsciiCheck(const char* f, size_t n) {
    if (n < 18 || f[0] != '~' || f[n - 1] != '\r') return false;
    size_t off = n - 5;
    uint16_t c;
    if (!hw(f, n, off, c) || c != ckSum(f, 1, n - 5)) return false;
    off = 9;
    uint16_t l;
    if (!hw(f, n, off, l) || ckLen(l & 0x0FFF) != l) return false;
    if ((size_t)(l & 0x0FFF) + 18 != n) return false;
    off = 7;
    uint8_t rtn;
    return hb(f, n, off, rtn) && rtn == 0;                   // 00 — «нормально»
}

// Одинаково для 0x25 и 0x20: два служебных байта, потом ячейки, температуры,
// ток, напряжение, остаток, служебный, полная, циклы, проектная ёмкость.
// У SEPLOS (вариант 0x20) за объявленными температурами идут ещё две —
// среды и ключей; если с объявленным числом сумма ячеек не сходится с
// напряжением, пробуем «+2».
static bool analogTry(const char* f, size_t n, PacePack& p, uint8_t extraTemps, uint8_t lead = 2) {
    size_t end = n - 5, off = 13;
    uint8_t b0, cells, temps, ud;
    uint16_t w;
    for (uint8_t i = 0; i < lead; i++) if (!hb(f, end, off, b0)) return false;
    if (!hb(f, end, off, cells)) return false;
    if (cells == 0 || cells > 16) return false;
    uint16_t mv[16];
    uint32_t sum = 0;
    for (uint8_t i = 0; i < cells; i++) { if (!hw(f, end, off, w)) return false; mv[i] = w; sum += w; }
    if (!hb(f, end, off, temps)) return false;
    temps += extraTemps;
    if (temps > 8) return false;
    int16_t t10[8];
    for (uint8_t i = 0; i < temps; i++) { if (!hw(f, end, off, w)) return false; t10[i] = (int16_t)w - 2730; }
    uint16_t cur, volt, rem, full, cyc, des = 0;
    if (!hw(f, end, off, cur) || !hw(f, end, off, volt) || !hw(f, end, off, rem) || !hb(f, end, off, ud) ||
        !hw(f, end, off, full) || !hw(f, end, off, cyc)) return false;
    hw(f, end, off, des);                                    // у старых версий может не быть
    // Здравый смысл: сумма ячеек ≈ напряжению батареи (в мВ или в 10 мВ) —
    // иначе разметка не та.
    float v = volt;
    if (fabsf((float)sum - v) > v * 0.05f) {
        if (fabsf((float)sum - v * 10) > v * 0.5f) return false;
        v *= 10;                                             // SEPLOS: напряжение в 10 мВ
    }
    if (v < 8000) return false;
    memcpy(p.mv, mv, sizeof(uint16_t) * cells);
    memcpy(p.t10, t10, sizeof(int16_t) * temps);
    p.cells = cells; p.temps = temps;
    p.curA = (int16_t)cur / 100.0f;
    p.voltV = v / 1000.0f;
    if (extraTemps) {
        // SEPLOS: после напряжения ёмкости и циклы лежат по-другому (проверено
        // на кадре из nkinnan/esphome-pace-bms — выходило 337,9 А·ч и 21519
        // циклов). Не гадаем: только то, что сверено.
        p.remAh = p.fullAh = p.designAh = 0;
        p.cycles = 0;
        p.soh = -1;
    } else {
        p.remAh = rem / 100.0f; p.fullAh = full / 100.0f; p.designAh = des / 100.0f;
        p.cycles = cyc;
        p.soh = (des > 0 && full > 0) ? fminf(100.0f, full * 100.0f / des) : -1;
    }
    p.ok = true;
    return true;
}

bool paceAsciiAnalog(const char* f, size_t n, PacePack& p) {
    if (!paceAsciiCheck(f, n)) return false;
    // Порядок: PACE и Pylontech (два служебных байта — флаг и адрес),
    // SEPLOS (+2 температуры), прошивки Pylontech с одним байтом-флагом.
    return analogTry(f, n, p, 0) || analogTry(f, n, p, 2) || analogTry(f, n, p, 0, 1);
}

bool paceAsciiStatus(const char* f, size_t n, PacePack& p) {
    if (!paceAsciiCheck(f, n)) return false;
    size_t end = n - 5, off = 13;
    uint8_t b0, b1, cells, temps, x, p1, p2, sys, cfg, flt, w1 = 0, w2 = 0;
    uint16_t bal;
    if (!hb(f, end, off, b0) || !hb(f, end, off, b1) || !hb(f, end, off, cells) || cells > 16) return false;
    for (uint8_t i = 0; i < cells; i++) if (!hb(f, end, off, x)) return false;
    if (!hb(f, end, off, temps) || temps > 8) return false;
    for (uint8_t i = 0; i < temps; i++) if (!hb(f, end, off, x)) return false;
    for (uint8_t i = 0; i < 3; i++) if (!hb(f, end, off, x)) return false;   // ток заряда, напряжение, ток разряда
    if (!hb(f, end, off, p1) || !hb(f, end, off, p2) || !hb(f, end, off, sys) || !hb(f, end, off, cfg) ||
        !hb(f, end, off, flt) || !hw(f, end, off, bal)) return false;
    hb(f, end, off, w1);
    hb(f, end, off, w2);
    p.prot = p1 | p2 << 8;
    p.warn = w1 | w2 << 8;
    p.fault = flt;
    p.balance = bal;
    p.chgMos = sys & (1 << 1);
    p.dsgMos = sys & (1 << 2);
    p.stOk = true;
    return true;
}

// Состояние по Pylontech (CID2 0x44, протокол 0x20). Так отвечает порт RS485
// BMS PACE, когда на её экране для RS485 выбран протокол PYLON (у батарей
// владельца — так, 23.10.2026). Разметка — по спецификации Pylontech RS485:
// флаг, адрес, по байту на ячейку (1 ниже, 2 выше), на температуру, токи и
// напряжение, затем Status1 (0 батарея выше, 1 ячейка ниже, 2 ток заряда,
// 4 ток разряда, 5 перегрев разряда, 6 перегрев заряда, 7 батарея ниже) и
// Status2 (1 ключ заряда, 2 ключ разряда). Балансировки в протоколе нет.
// На живом кадре не проверено — первый кадр сохраняется сырым.
bool paceAsciiStatusPylon(const char* f, size_t n, PacePack& p) {
    if (!paceAsciiCheck(f, n)) return false;
    size_t end = n - 5, off = 13;
    uint8_t b0, b1, cells, temps, x, ci, mv, di, s1, s2;
    if (!hb(f, end, off, b0) || !hb(f, end, off, b1) || !hb(f, end, off, cells) || cells == 0 || cells > 16) return false;
    uint16_t warn = 0;
    for (uint8_t i = 0; i < cells; i++) {
        if (!hb(f, end, off, x)) return false;
        if (x == 1) warn |= 1 << 1;
        if (x == 2) warn |= 1 << 0;
    }
    if (!hb(f, end, off, temps) || temps > 8) return false;
    for (uint8_t i = 0; i < temps; i++) {
        if (!hb(f, end, off, x)) return false;
        if (x == 2) warn |= 1 << 8;
        if (x == 1) warn |= 1 << 10;
    }
    if (!hb(f, end, off, ci) || !hb(f, end, off, mv) || !hb(f, end, off, di) || !hb(f, end, off, s1) || !hb(f, end, off, s2)) return false;
    if (ci == 2) warn |= 1 << 4;
    if (mv == 2) warn |= 1 << 2;
    if (mv == 1) warn |= 1 << 3;
    if (di == 2) warn |= 1 << 5;
    uint16_t prot = 0;
    if (s1 & 0x01) prot |= 1 << 2;
    if (s1 & 0x02) prot |= 1 << 1;
    if (s1 & 0x04) prot |= 1 << 4;
    if (s1 & 0x10) prot |= 1 << 5;
    if (s1 & 0x20) prot |= 1 << 9;
    if (s1 & 0x40) prot |= 1 << 8;
    if (s1 & 0x80) prot |= 1 << 3;
    p.warn = warn;
    p.prot = prot;
    p.fault = 0;
    p.balance = 0;
    p.chgMos = s2 & 0x02;
    p.dsgMos = s2 & 0x04;
    p.stOk = true;
    return true;
}

// ------------------------------------------------------------------ Modbus

uint16_t paceCrc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (uint8_t b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

size_t paceModbusRequest(uint8_t* o, uint8_t addr, uint16_t reg, uint16_t count) {
    o[0] = addr; o[1] = 0x03; o[2] = reg >> 8; o[3] = reg; o[4] = count >> 8; o[5] = count;
    uint16_t c = paceCrc16(o, 6);
    o[6] = c; o[7] = c >> 8;
    return 8;
}

// Регистры 0–36 одним запросом (руководство PACE Modbus V1.3, 4.1).
bool paceModbusParse(const uint8_t* rx, size_t n, uint8_t addr, PacePack& p) {
    if (n < 5 + 74 || rx[0] != addr || rx[1] != 0x03 || rx[2] != 74) return false;
    uint16_t c = paceCrc16(rx, 3 + 74);
    if (rx[3 + 74] != (c & 0xFF) || rx[4 + 74] != (c >> 8)) return false;
    auto r = [&](int i) -> uint16_t { return rx[3 + i * 2] << 8 | rx[4 + i * 2]; };
    uint16_t volt10 = r(1);
    if (volt10 < 800) return false;
    p.curA = (int16_t)r(0) / 100.0f;
    p.voltV = volt10 / 100.0f;
    p.soh = r(3);
    p.remAh = r(4) / 100.0f; p.fullAh = r(5) / 100.0f; p.designAh = r(6) / 100.0f;
    p.cycles = r(7);
    p.warn = r(9); p.prot = r(10);
    uint16_t sf = r(11);
    p.fault = sf & 0xFF;
    p.chgMos = sf & (1 << 10);
    p.dsgMos = sf & (1 << 11);
    p.balance = r(12);
    p.cells = 0;
    for (int i = 0; i < 16; i++) { p.mv[i] = r(15 + i); if (p.mv[i]) p.cells = i + 1; }
    p.temps = 0;
    for (int i = 0; i < 6; i++) {
        int16_t t = (int16_t)r(31 + i);
        if (t > -500 && t < 1500) p.t10[p.temps++] = t;   // «или недействительно» у MOS и среды
    }
    p.ok = p.stOk = p.cells > 0;
    return p.ok;
}

// ------------------------------------------------------------------ обмен
// Дальше — железо и задача: только в сборке HS35. Разбор выше — везде,
// его проверяют на компьютере.

#if defined(TARGET_HS35)

#include "state.h"
#include "tg.h"
#include <Preferences.h>

static const int PIN_RX = 18, PIN_TX = 17;
static const uint32_t BAUD = 9600;
static const uint8_t  NPACK = 4;
static const uint32_t POLL_MS = 5000;

enum Proto : uint8_t { P_NONE, P_V25, P_MODBUS, P_V20 };
static const char* const PROTO_NAME[] = { "—", "PACE ASCII 0x25", "PACE Modbus RTU", "Pylontech RS485 (0x20)" };

static HardwareSerial s_ser(1);
static SemaphoreHandle_t s_mx;
static volatile uint8_t s_proto = P_NONE;
static uint8_t  s_addr[NPACK];
static uint8_t  s_n = 0;                       // найдено батарей
static PacePack s_pk[NPACK];
static uint8_t  s_fail[NPACK];
static String   s_status = "ещё не искал";
static String   s_raw[3];                     // ответы, которые не разобрались
static uint8_t  s_nRaw = 0;
static uint32_t s_okMs = 0;

static void rememberRaw(const String& r) {
    for (uint8_t i = 0; i < s_nRaw; i++) if (s_raw[i] == r) return;
    if (s_nRaw < 3) s_raw[s_nRaw++] = r;
}

static String hexDump(const uint8_t* d, size_t n) {
    String r;
    for (size_t i = 0; i < n && i < 100; i++) { r += HEX_[d[i] >> 4]; r += HEX_[d[i] & 15]; }
    return r;
}

// ASCII-запрос и ответ до \r. Эхо своего запроса (бывает у модулей RS485)
// пропускается. 0 — ответа нет.
static size_t asciiXfer(uint8_t ver, uint8_t addr, uint8_t cid2, char* rx, size_t cap) {
    char tx[24];
    size_t tn = paceAsciiRequest(tx, sizeof(tx), ver, addr, cid2);
    while (s_ser.available()) s_ser.read();
    s_ser.write((const uint8_t*)tx, tn);
    s_ser.flush();
    size_t n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 700) {
        if (!s_ser.available()) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        char c = s_ser.read();
        if (n == 0 && c != '~') continue;
        if (n < cap - 1) rx[n++] = c;
        if (c == '\r') {
            rx[n] = 0;
            if (n == tn && !memcmp(rx, tx, tn)) { n = 0; continue; }   // эхо
            return n;
        }
    }
    if (n) { rx[n] = 0; rememberRaw("обрыв: " + String(rx).substring(0, 120)); }
    return 0;
}

static size_t modbusXfer(uint8_t addr, uint16_t reg, uint16_t cnt, uint8_t* rx, size_t cap) {
    uint8_t tx[8];
    paceModbusRequest(tx, addr, reg, cnt);
    while (s_ser.available()) s_ser.read();
    s_ser.write(tx, 8);
    s_ser.flush();
    size_t want = 5 + cnt * 2, n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 700 && n < want && n < cap) {
        if (!s_ser.available()) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        rx[n++] = s_ser.read();
        if (n == 8 && !memcmp(rx, tx, 8)) n = 0;            // эхо
        if (n == 5 && rx[1] == 0x83) return 0;              // ошибка Modbus — адрес жив, но отказал
    }
    return n;
}

// Один опрос батареи — true, если пришли данные.
static bool pollPack(uint8_t proto, uint8_t addr, PacePack& p, bool probe) {
    static char abuf[320];
    static uint8_t mbuf[96];
    PacePack q = p;
    if (proto == P_MODBUS) {
        size_t n = modbusXfer(addr, 0, 37, mbuf, sizeof(mbuf));
        if (!n) return false;
        if (!paceModbusParse(mbuf, n, addr, q)) { if (probe) rememberRaw("Modbus " + String(addr) + ": " + hexDump(mbuf, n)); return false; }
    } else {
        uint8_t ver = proto == P_V25 ? 0x25 : 0x20;
        size_t n = asciiXfer(ver, addr, 0x42, abuf, sizeof(abuf));
        if (!n) return false;
        if (!paceAsciiAnalog(abuf, n, q)) { if (probe) rememberRaw(String(abuf).substring(0, 200)); return false; }
        vTaskDelay(pdMS_TO_TICKS(120));                      // пауза между кадрами — от 100 мс
        n = asciiXfer(ver, addr, 0x44, abuf, sizeof(abuf));
        if (n) {
            // У PACE 0x25 и у Pylontech (0x20) состояние разное по разметке.
            bool ok = proto == P_V25 ? paceAsciiStatus(abuf, n, q) : paceAsciiStatusPylon(abuf, n, q);
            static bool keptPylon = false;
            if (proto == P_V20 && !keptPylon) { keptPylon = true; rememberRaw("состояние 0x44 (Pylontech, для сверки): " + String(abuf).substring(0, 200)); }
            if (!ok && probe) rememberRaw(String(abuf).substring(0, 200));
        }
    }
    q.addr = addr;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    p = q;
    xSemaphoreGive(s_mx);
    return true;
}

static void save() {
    Preferences pr;
    if (!pr.begin("pace", false)) return;
    pr.putUChar("proto", s_proto);
    pr.putBytes("addr", s_addr, s_n);
    pr.end();
}

// Поиск: протоколы по очереди, адреса 0–15; нашёлся — остальные адреса тем же.
static void probe() {
    // Pylontech первым: у батарей владельца на экране для RS485 выбран PYLON.
    static const uint8_t ORDER[] = { P_V20, P_V25, P_MODBUS };
    for (uint8_t pi = 0; pi < sizeof(ORDER); pi++) {
        uint8_t pr = ORDER[pi];
        for (uint8_t a = 0; a < 16; a++) {
            if (pr == P_MODBUS && a == 0) continue;          // 0 в Modbus — широковещательный
            s_status = String("ищу BMS: ") + PROTO_NAME[pr] + ", адрес " + a;
            PacePack p;
            if (!pollPack(pr, a, p, true)) { vTaskDelay(pdMS_TO_TICKS(120)); continue; }
            s_proto = pr;
            s_n = 0;
            s_addr[s_n] = a; s_pk[s_n] = p; s_fail[s_n] = 0; s_n++;
            for (uint8_t b = a + 1; b < 16 && s_n < NPACK; b++) {
                s_status = String("нашёл батарею ") + a + ", ищу ещё: адрес " + b;
                PacePack q;
                if (pollPack(pr, b, q, false)) { s_addr[s_n] = b; s_pk[s_n] = q; s_fail[s_n] = 0; s_n++; }
                vTaskDelay(pdMS_TO_TICKS(120));
            }
            save();
            s_status = "на связи";
            Serial.printf("[pace] %s, батарей: %u\n", PROTO_NAME[pr], s_n);
            tgNotify("🔋 BMS батарей на связи: " + String(PROTO_NAME[pr]) + ", батарей " + String(s_n));
            return;
        }
    }
    s_status = "BMS не отвечает — проверьте провода A/B и порт RS485";
}

static void publish() {
    // Ячейки всех батарей подряд: 1–16 — первая, 17–32 — вторая.
    stateUpdate([](BmsState& b) {
        uint8_t n = 0, tn = 0;
        uint16_t mx = 0, mn = 65535;
        uint8_t mxNo = 0, mnNo = 0;
        int16_t tMax = -1000, tMin = 1000;
        bool chg = true, dsg = true;
        memset(b.balance, 0, sizeof(b.balance));
        for (uint8_t k = 0; k < s_n; k++) {
            const PacePack& p = s_pk[k];
            if (!p.ok) continue;
            for (uint8_t i = 0; i < p.cells && n < MAX_CELLS; i++, n++) {
                b.cellMv[n] = p.mv[i];
                if (p.mv[i] > mx) { mx = p.mv[i]; mxNo = n + 1; }
                if (p.mv[i] && p.mv[i] < mn) { mn = p.mv[i]; mnNo = n + 1; }
                if (p.balance >> i & 1) b.balance[n / 8] |= 1 << (n % 8);
            }
            for (uint8_t i = 0; i < p.temps && i < 4; i++) {          // первые 4 — ячейки
                int16_t t = p.t10[i] / 10;
                if (tn < MAX_TEMPS) b.cellTemp[tn++] = t;
                if (t > tMax) tMax = t;
                if (t < tMin) tMin = t;
            }
            if (p.stOk) { chg = chg && p.chgMos; dsg = dsg && p.dsgMos; }
        }
        b.cellCount = n;
        b.tempCount = tn;
        if (n) { b.cellMaxMv = mx; b.cellMinMv = mn; b.cellMaxNo = mxNo; b.cellMinNo = mnNo; }
        if (tn) { b.tempMax = tMax; b.tempMin = tMin; }
        b.chargeMos = chg;
        b.dischargeMos = dsg;
    });
}

static void task(void*) {
    s_ser.begin(BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
    Preferences pr;
    if (pr.begin("pace", true)) {
        s_proto = pr.getUChar("proto", P_NONE);
        s_n = pr.getBytesLength("addr");
        if (s_n > NPACK) s_n = NPACK;
        if (s_n) pr.getBytes("addr", s_addr, s_n);
        pr.end();
    }
    uint16_t allFail = 0;
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        if (s_proto == P_NONE || !s_n) {
            probe();
            if (s_proto == P_NONE) { vTaskDelay(pdMS_TO_TICKS(120000)); continue; }   // нет — раз в 2 минуты
        }
        bool any = false;
        for (uint8_t k = 0; k < s_n; k++) {
            if (pollPack(s_proto, s_addr[k], s_pk[k], false)) { s_fail[k] = 0; any = true; }
            else if (++s_fail[k] >= 6) s_pk[k].ok = false;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (any) { allFail = 0; s_okMs = millis(); s_status = "на связи"; publish(); }
        else if (++allFail >= 24) {                                       // 2 минуты молчит — искать заново
            s_status = "связь с BMS потеряна, ищу заново";
            s_proto = P_NONE; s_n = 0; allFail = 0;
            stateUpdate([](BmsState& b) { b.cellCount = 0; b.tempCount = 0; });
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void paceStart() {
    s_mx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(task, "pace", 6144, nullptr, 3, nullptr, 0);
}

// ------------------------------------------------------------------ выдача

static const char* const WARN_N[16] = {
    "ячейка выше нормы", "ячейка ниже нормы", "батарея выше нормы", "батарея ниже нормы",
    "ток заряда велик", "ток разряда велик", "", "",
    "горячо при заряде", "горячо при разряде", "холодно для заряда", "холодно для разряда",
    "жарко вокруг", "холодно вокруг", "силовые ключи горячие", "мало заряда" };
static const char* const PROT_N[16] = {
    "перезаряд ячейки", "переразряд ячейки", "перенапряжение батареи", "глубокий разряд батареи",
    "перегрузка по заряду", "перегрузка по разряду", "короткое замыкание", "перенапряжение зарядного",
    "перегрев при заряде", "перегрев при разряде", "холод при заряде", "холод при разряде",
    "перегрев ключей", "перегрев среды", "холод среды", "" };
static const char* const FAULT_N[8] = {
    "ключ заряда", "ключ разряда", "датчик температуры", "связь", "ячейка", "измерение", "", "" };

static String flags(uint16_t v, const char* const* names, uint8_t bits) {
    String r;
    for (uint8_t i = 0; i < bits; i++)
        if ((v >> i & 1) && names[i][0]) r += String(r.length() ? ", " : "") + names[i];
    return r;
}

static String esc(String s) { s.replace("\"", "'"); return s; }

String paceJson() {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    String j = "{\"proto\":\"" + String(PROTO_NAME[s_proto]) + "\",\"status\":\"" + esc(s_status) +
               "\",\"age\":" + String(s_okMs ? (millis() - s_okMs) / 1000 : 0) + ",\"packs\":[";
    for (uint8_t k = 0; k < s_n; k++) {
        const PacePack& p = s_pk[k];
        if (k) j += ",";
        j += "{\"addr\":" + String(p.addr) + ",\"ok\":" + (p.ok ? "true" : "false") +
             ",\"v\":" + String(p.voltV, 2) + ",\"i\":" + String(p.curA, 2) +
             ",\"rem\":" + String(p.remAh, 1) + ",\"full\":" + String(p.fullAh, 1) + ",\"design\":" + String(p.designAh, 1) +
             ",\"cycles\":" + String(p.cycles) + ",\"soh\":" + String(p.soh, 0) +
             ",\"chg\":" + (p.chgMos ? "true" : "false") + ",\"dsg\":" + (p.dsgMos ? "true" : "false") +
             ",\"bal\":" + String(p.balance) + ",\"cells\":[";
        for (uint8_t i = 0; i < p.cells; i++) j += String(i ? "," : "") + p.mv[i];
        j += "],\"t\":[";
        for (uint8_t i = 0; i < p.temps; i++) j += String(i ? "," : "") + String(p.t10[i] / 10.0f, 1);
        j += "],\"warn\":\"" + flags(p.warn, WARN_N, 16) + "\",\"prot\":\"" + flags(p.prot, PROT_N, 15) +
             "\",\"fault\":\"" + flags(p.fault, FAULT_N, 6) + "\"}";
    }
    j += "],\"raw\":[";
    for (uint8_t i = 0; i < s_nRaw; i++) j += String(i ? "," : "") + "\"" + esc(s_raw[i]) + "\"";
    xSemaphoreGive(s_mx);
    return j + "]}";
}

String paceAlarm() {
    String r;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (uint8_t k = 0; k < s_n; k++) {
        const PacePack& p = s_pk[k];
        if (!p.ok) continue;
        String pr = flags(p.prot & 0x7FFF, PROT_N, 15), f = flags(p.fault, FAULT_N, 6);
        if (pr.length() || f.length())
            r += String(r.length() ? "\n" : "") + "батарея " + String(k + 1) + ": " + pr + (pr.length() && f.length() ? ", " : "") +
                 (f.length() ? "неисправность — " + f : String(""));
    }
    xSemaphoreGive(s_mx);
    return r;
}

String paceText() {
    if (!s_n) return "🔋 Ячейки: BMS не подключена — " + s_status;
    String r = "🔋 Ячейки";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (uint8_t k = 0; k < s_n; k++) {
        const PacePack& p = s_pk[k];
        r += "\n\nбатарея " + String(k + 1) + " (адрес " + String(p.addr) + ")";
        if (!p.ok) { r += ": нет ответа"; continue; }
        uint16_t mx = 0, mn = 65535;
        for (uint8_t i = 0; i < p.cells; i++) { if (p.mv[i] > mx) mx = p.mv[i]; if (p.mv[i] < mn) mn = p.mv[i]; }
        r += ": " + String(p.voltV, 2) + " В, " + String(p.curA, 1) + " А\nячейки " + String(mn) + "…" + String(mx) +
             " мВ, разброс " + String(mx - mn) + " мВ";
        if (p.balance) r += ", балансируются: " + String(__builtin_popcount(p.balance));
        r += "\nциклов " + String(p.cycles);
        if (p.soh >= 0) r += ", здоровье " + String(p.soh, 0) + " %";
        String w = flags(p.warn, WARN_N, 16), pr = flags(p.prot, PROT_N, 15), f = flags(p.fault, FAULT_N, 6);
        if (pr.length()) r += "\n❗ защита: " + pr;
        if (w.length())  r += "\n⚠️ " + w;
        if (f.length())  r += "\n❗ неисправность: " + f;
    }
    xSemaphoreGive(s_mx);
    return r;
}

#else
void   paceStart() {}
String paceJson() { return "{\"proto\":\"—\",\"status\":\"только в сборке HS35\",\"packs\":[],\"raw\":[]}"; }
String paceText() { return "Ячейки читаются только в сборке HS35."; }
String paceAlarm() { return ""; }
#endif
