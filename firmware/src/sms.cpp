// SMS через SIM800L. Описание и подключение — в sms.h.
#include "sms.h"

// ------------------------------------------------------------------ разбор

static void put4(String& o, uint16_t v) {
    static const char* H = "0123456789ABCDEF";
    o += H[v >> 12]; o += H[(v >> 8) & 15]; o += H[(v >> 4) & 15]; o += H[v & 15];
}

// UTF-8 → коды; продвигает i. 0xFFFF — испорченное.
static uint32_t cp(const String& s, size_t& i) {
    uint8_t c = s[i++];
    if (c < 0x80) return c;
    int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    uint32_t v = n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
    if (!n) return 0xFFFF;
    for (int k = 0; k < n; k++) {
        if (i >= s.length() || (s[i] & 0xC0) != 0x80) return 0xFFFF;
        v = v << 6 | (s[i++] & 0x3F);
    }
    return v;
}

static void utf8(String& o, uint32_t v) {
    if (v < 0x80) o += (char)v;
    else if (v < 0x800) { o += (char)(0xC0 | v >> 6); o += (char)(0x80 | (v & 0x3F)); }
    else { o += (char)(0xE0 | v >> 12); o += (char)(0x80 | ((v >> 6) & 0x3F)); o += (char)(0x80 | (v & 0x3F)); }
}

String smsUcs2Enc(const String& s) {
    String o;
    for (size_t i = 0; i < s.length();) {
        uint32_t v = cp(s, i);
        if (v > 0xFFFF || v == 0xFE0F || v == 0xFFFF) continue;   // эмодзи и их вариации в SMS не нужны
        put4(o, (uint16_t)v);
    }
    return o;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

String smsUcs2Dec(const String& h) {
    if (h.length() < 4 || h.length() % 4) return "";
    String o;
    for (size_t i = 0; i < h.length(); i += 4) {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) { int d = hexv(h[i + k]); if (d < 0) return ""; v = v << 4 | d; }
        utf8(o, v);
    }
    return o;
}

int smsUtf8Len(const String& s) {
    int n = 0;
    for (size_t i = 0; i < s.length(); i++) if ((s[i] & 0xC0) != 0x80) n++;
    return n;
}

String smsUtf8Cut(const String& s, int from, int n) {
    int k = 0;
    size_t a = s.length(), b = s.length();
    for (size_t i = 0; i <= s.length(); i++) {
        if (i < s.length() && (s[i] & 0xC0) == 0x80) continue;
        if (k == from) a = i;
        if (k == from + n) { b = i; break; }
        k++;
    }
    return a < b ? s.substring(a, b) : String("");
}

static String digits(const String& s) {
    String d;
    for (size_t i = 0; i < s.length(); i++) if (isdigit((uint8_t)s[i])) d += s[i];
    return d;
}

bool smsNumMatch(const String& a, const String& b) {
    String x = digits(a), y = digits(b);
    if (x.length() < 10 || y.length() < 10) return false;
    return x.substring(x.length() - 10) == y.substring(y.length() - 10);
}

int smsCsqDbm(const String& r) {
    int p = r.indexOf("+CSQ:");
    if (p < 0) return 0;
    int v = r.substring(p + 5).toInt();
    return v > 0 && v < 32 ? -113 + 2 * v : 0;
}

int smsCreg(const String& r) {
    int p = r.indexOf("+CREG:");
    if (p < 0) return -1;
    int c = r.indexOf(',', p);
    return c < 0 ? -1 : (int)r.substring(c + 1).toInt();
}

static String quoted(const String& s, int from, int nth) {   // n-е «"…"» начиная с from
    int a = from;
    for (int k = 0; k <= nth; k++) {
        a = s.indexOf('"', a);
        if (a < 0) return "";
        int b = s.indexOf('"', a + 1);
        if (b < 0) return "";
        if (k == nth) return s.substring(a + 1, b);
        a = b + 1;
    }
    return "";
}

String smsCops(const String& r) {
    int p = r.indexOf("+COPS:");
    return p < 0 ? String("") : quoted(r, p, 0);
}

String smsCusd(const String& l) {
    int p = l.indexOf("+CUSD:");
    if (p < 0) return "";
    String t = quoted(l, p, 0);
    String d = smsUcs2Dec(t);
    return d.length() ? d : t;
}

String smsLower(const String& s) {
    String o;
    for (size_t i = 0; i < s.length();) {
        uint32_t v = cp(s, i);
        if (v >= 'A' && v <= 'Z') v += 32;
        else if (v >= 0x410 && v <= 0x42F) v += 32;
        else if (v == 0x401) v = 0x451;
        if (v <= 0xFFFF) utf8(o, v);
    }
    return o;
}

// ------------------------------------------------------------------ модем

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || defined(SMS_HOST)
void   smsStart() {}
void   smsAlert(const String&) {}
String smsJson() { return "{\"on\":false}"; }
String smsSet(int, const String*, int, const String*) { return "модема на этой плате нет"; }
void   smsTest() {}
void   smsBalance() {}
String smsLine() { return ""; }
#else

#include <Preferences.h>
#include <time.h>
#include "state.h"
#include "stats.h"
#include "cfg.h"
#include "tg.h"
#include "json.h"
#include "events.h"

static const int PIN_TX = 5, PIN_RX = 6, PIN_RST = 7;
static const uint32_t BAUD = 9600;
static const uint8_t NNUM = 3, NPEND = 8, NLOG = 6;
static const int32_t TZ_SEC = 3 * 3600;

static HardwareSerial s_mod(2);
static SemaphoreHandle_t s_mx;
static TaskHandle_t s_task = nullptr;

// Настройки (NVS "sms").
static uint8_t s_mode = 1, s_delay = 3;
static String  s_nums, s_ussd = "*100#";

// Состояние модема.
static volatile bool s_present = false;
static int     s_reg = -1, s_dbm = 0;
static String  s_op, s_err, s_last, s_bal;
static uint32_t s_sent = 0;
static volatile bool s_testReq = false, s_balReq = false, s_newSms = false;
static String  s_balTo;                        // баланс просили по SMS — ответить сюда
static String  s_cusd;                         // последний ответ USSD

struct Pend { uint32_t at; String text; };
static Pend    s_pend[NPEND];
static uint8_t s_nPend = 0;
static String  s_offLog[NLOG];                 // что ушло SMS, пока бот молчал
static uint8_t s_nOff = 0;
static uint32_t s_offAt = 0;

static String hm() {
    time_t t = time(nullptr) + TZ_SEC;
    struct tm lt;
    gmtime_r(&t, &lt);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", lt.tm_hour, lt.tm_min);
    return b;
}

static uint8_t numList(String* out) {
    uint8_t n = 0;
    String cur;
    for (size_t i = 0; i <= s_nums.length() && n < NNUM; i++) {
        char c = i < s_nums.length() ? s_nums[i] : ',';
        if (c == ',' || c == ';') { if (digits(cur).length() >= 10) out[n++] = cur; cur = ""; }
        else if (c == '+' || isdigit((uint8_t)c)) cur += c;           // «+7 978 123-45-67» → «+79781234567»
    }
    return n;
}

// ---- обмен
static String s_line;

// Строки, которые модем шлёт сам — не ответ на команду.
static bool unsolicited(const String& l) {
    if (l.startsWith("+CUSD:")) { s_cusd = l; return true; }
    if (l.startsWith("+CMTI:")) { s_newSms = true; return true; }
    if (l == "RING" || l.startsWith("+CLIP:") || l == "NO CARRIER" || l.startsWith("+CPIN:") ||
        l == "Call Ready" || l == "SMS Ready" || l.startsWith("+CFUN:") || l.startsWith("UNDER-VOLTAGE") ||
        l.startsWith("OVER-VOLTAGE")) {
        if (l.startsWith("UNDER-VOLTAGE")) s_err = "модему не хватает питания — нужен блок 5 В / 2 А";
        return true;
    }
    return false;
}

static bool readLine(String& out, uint32_t toMs) {
    uint32_t t0 = millis();
    while (millis() - t0 < toMs) {
        while (s_mod.available()) {
            char c = s_mod.read();
            if (c == '\r') continue;
            if (c == '\n') {
                if (!s_line.length()) continue;
                out = s_line;
                s_line = "";
                return true;
            }
            if (s_line.length() < 600) s_line += c;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void drain() {
    String l;
    while (readLine(l, 30)) unsolicited(l);
}

// 1 — OK, 0 — ERROR, −1 — молчит. Промежуточные строки — в resp.
static int at(const String& cmd, String* resp = nullptr, uint32_t toMs = 2000) {
    drain();
    s_mod.print(cmd);
    s_mod.print("\r");
    uint32_t t0 = millis();
    String l;
    while (millis() - t0 < toMs) {
        if (!readLine(l, toMs - (millis() - t0))) break;
        if (l == cmd) continue;                                 // эхо
        if (l == "OK") return 1;
        if (l == "ERROR" || l.startsWith("+CME ERROR") || l.startsWith("+CMS ERROR")) { s_err = cmd + ": " + l; return 0; }
        if (unsolicited(l)) continue;
        if (resp) { *resp += l; *resp += '\n'; }
    }
    return -1;
}

static bool initModem() {
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(200));
    digitalWrite(PIN_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(4000));
    bool ok = false;
    for (int i = 0; i < 10 && !ok; i++) ok = at("AT", nullptr, 800) == 1;   // автоподстройка скорости
    if (!ok) return false;
    at("ATE0");
    at("AT+CMEE=2");
    at("AT+CMGF=1");                                            // текстовый режим SMS
    at("AT+CSCS=\"GSM\"");
    at("AT+CNMI=2,1,0,0,0");                                    // о новых SMS — «+CMTI»
    return true;
}

static void status() {
    String r;
    if (at("AT+CSQ", &r) == 1) s_dbm = smsCsqDbm(r);
    r = "";
    if (at("AT+CREG?", &r) == 1) s_reg = smsCreg(r);
    r = "";
    if (at("AT+COPS?", &r) == 1) s_op = smsCops(r);
}

// Одна SMS до 70 знаков в UCS2.
static bool sendOne(const String& num, const String& text) {
    if (at("AT+CSCS=\"UCS2\"") != 1) return false;
    at("AT+CSMP=17,167,0,8");                                   // кодировка UCS2
    drain();
    s_mod.print("AT+CMGS=\"" + smsUcs2Enc(num) + "\"\r");
    uint32_t t0 = millis();
    bool prompt = false;
    while (millis() - t0 < 5000 && !prompt) {
        while (s_mod.available()) if (s_mod.read() == '>') prompt = true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool ok = false;
    if (prompt) {
        s_mod.print(smsUcs2Enc(text));
        s_mod.write(0x1A);
        t0 = millis();
        String l;
        while (millis() - t0 < 60000 && readLine(l, 60000 - (millis() - t0))) {
            if (l == "OK") { ok = true; break; }
            if (l == "ERROR" || l.startsWith("+CMS ERROR")) { s_err = "отправка: " + l; break; }
            unsolicited(l);
        }
    } else {
        s_mod.write(0x1B);                                      // отменить ввод
        s_err = "модем не принял номер";
    }
    at("AT+CSCS=\"GSM\"");
    return ok;
}

// На все номера (или на один), длинное — до трёх SMS по 70 знаков.
static bool sendSms(const String& text, const String* only = nullptr) {
    String nums[NNUM];
    uint8_t n = only ? 1 : numList(nums);
    if (only) nums[0] = *only;
    if (!n || !s_present) return false;
    int len = smsUtf8Len(text), parts = len <= 70 ? 1 : min(3, (len + 66) / 67);
    bool all = true;
    for (uint8_t k = 0; k < n; k++) {
        for (int p = 0; p < parts; p++) {
            String part = parts == 1 ? text : String(p + 1) + "/" + String(parts) + " " + smsUtf8Cut(text, p * 67, 67);
            bool ok = sendOne(nums[k], part);
            all = all && ok;
            String masked = nums[k].length() > 4 ? "…" + nums[k].substring(nums[k].length() - 4) : nums[k];
            xSemaphoreTake(s_mx, portMAX_DELAY);
            s_last = hm() + " → " + masked + (ok ? " отправлено" : " не ушло");
            if (ok) s_sent++;
            xSemaphoreGive(s_mx);
            Serial.printf("[sms] %s: %s\n", ok ? "отправлено" : "не ушло", part.c_str());
        }
    }
    return all;
}

static String dec1(float v) { String r = String(v, 1); r.replace('.', ','); return r; }

static String statusText() {
    InvState v = invSnapshot();
    BmsState b = stateSnapshot();
    String r = !v.fresh ? "Нет связи с инвертором. " : v.gridOn ? "Сеть есть. " : "Сети нет. ";
    if (b.fresh) {
        r += "Батарея ";
        if (b.socKnown) r += String(b.soc, 0) + " %, ";
        r += dec1(b.voltage) + " В, " + String(b.current, 0) + " А";
        float avg = statsBattAvgA();
        uint32_t eta = b.socKnown ? battEtaS(b.soc, b.capAh, avg, cfg().tgSocLow) : 0;
        if (eta && avg < 0) r += ", хватит на " + String(eta / 3600) + " ч " + String(eta / 60 % 60) + " мин";
        r += ". ";
    }
    if (v.fresh) r += "Солнце " + String(v.pvW, 0) + " Вт, дом " + String(v.outW, 0) + " Вт.";
    return r;
}

static void ussd(const String& code) {
    s_cusd = "";
    if (at("AT+CUSD=1,\"" + code + "\"", nullptr, 5000) != 1) { s_bal = "оператор не принял " + code; return; }
    uint32_t t0 = millis();
    String l;
    while (millis() - t0 < 20000 && !s_cusd.length()) if (readLine(l, 1000)) unsolicited(l);
    String b = s_cusd.length() ? smsCusd(s_cusd) : String("нет ответа на ") + code;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_bal = hm() + ": " + b;
    xSemaphoreGive(s_mx);
}

// Входящие: «+CMGL: 1,"REC UNREAD","+7978…","","26/11/02,21:30:00+12"», следом текст.
static void readInbox() {
    s_newSms = false;
    String r;
    if (at("AT+CMGL=\"REC UNREAD\"", &r, 10000) != 1) return;
    int p = 0;
    while ((p = r.indexOf("+CMGL:", p)) >= 0) {
        String from = quoted(r, p, 1);
        int a = r.indexOf('\n', p);
        if (a < 0) break;
        int b = r.indexOf("+CMGL:", a);
        String text = r.substring(a + 1, b < 0 ? r.length() : b);
        text.trim();
        String dec = smsUcs2Dec(text);
        if (dec.length()) text = dec;
        p = a;
        String nums[NNUM];
        uint8_t n = numList(nums);
        bool known = false;
        for (uint8_t k = 0; k < n; k++) if (smsNumMatch(from, nums[k])) known = true;
        Serial.printf("[sms] входящее от %s%s: %s\n", from.c_str(), known ? "" : " (не из списка)", text.c_str());
        if (!known) continue;
        String c = smsLower(text);
        c.trim();
        if (c == "статус" || c == "status" || c == "?" || c == "с" || c == "s") sendSms(statusText(), &from);
        else if (c == "баланс" || c == "balance" || c == "б") { s_balTo = from; s_balReq = true; }
        else sendSms("Команды: статус, баланс, помощь.", &from);
    }
    at("AT+CMGDA=\"DEL READ\"", nullptr, 5000);
}

// Тревоги: режим 2 — сразу; режим 1 — подождать N минут: если бот за это
// время достучался, сообщение ушло через него, SMS не нужна.
static void pendings() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < s_nPend;) {
        Pend p;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        p = s_pend[i];
        xSemaphoreGive(s_mx);
        bool due = s_mode == 2 || now - p.at >= (uint32_t)s_delay * 60000;
        if (!due) { i++; continue; }
        uint32_t ok = tgLastOkMs();
        bool delivered = s_mode == 1 && tgOnline() >= 0 && ok && (int32_t)(ok - p.at) > 0;
        if (!delivered && sendSms(p.text) && s_mode == 1) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            if (s_nOff < NLOG) s_offLog[s_nOff++] = hm() + " " + p.text;
            s_offAt = millis() | 1;
            xSemaphoreGive(s_mx);
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        for (uint8_t k = i + 1; k < s_nPend; k++) s_pend[k - 1] = s_pend[k];
        s_nPend--;
        xSemaphoreGive(s_mx);
    }
    // Бот снова на связи — рассказать, что уходило SMS без него.
    uint32_t ok = tgLastOkMs();
    if (s_nOff && ok && (int32_t)(ok - s_offAt) > 0) {
        String t = "📵 Пока бот был без интернета, ушли SMS:";
        xSemaphoreTake(s_mx, portMAX_DELAY);
        for (uint8_t k = 0; k < s_nOff; k++) t += "\n• " + s_offLog[k];
        s_nOff = 0;
        xSemaphoreGive(s_mx);
        tgNotify(t);
    }
}

static void task(void*) {
    s_mod.begin(BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
    uint32_t lastStatus = 0, lastInbox = 0, lastTry = 0;
    uint8_t fails = 0;
    for (;;) {
        if (!s_present) {
            if (lastTry && millis() - lastTry < 120000) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            lastTry = millis() | 1;
            if (initModem()) {
                s_present = true;
                fails = 0;
                s_err = "";
                Serial.println("[sms] модем SIM800 на связи");
                status();
                at("AT+CMGDA=\"DEL READ\"", nullptr, 5000);
            } else {
                xSemaphoreTake(s_mx, portMAX_DELAY);
                s_err = "модем не отвечает";
                s_nPend = 0;                                    // слать некуда
                xSemaphoreGive(s_mx);
                continue;
            }
        }
        uint32_t now = millis();
        if (now - lastStatus > 30000) {
            lastStatus = now;
            int r = at("AT", nullptr, 1500);
            if (r == -1 && ++fails >= 3) { s_present = false; s_reg = -1; s_dbm = 0; lastTry = 0; continue; }
            if (r == 1) { fails = 0; status(); }
        }
        if (s_newSms || now - lastInbox > 20000) { lastInbox = now; readInbox(); }
        pendings();
        if (s_testReq) { s_testReq = false; sendSms("Проверка: модуль солнечной станции шлёт SMS. " + statusText()); }
        if (s_balReq) {
            s_balReq = false;
            ussd(s_ussd);
            if (s_balTo.length()) { String to = s_balTo; s_balTo = ""; sendSms(s_bal, &to); }
        }
        drain();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void load() {
    Preferences p;
    if (!p.begin("sms", true)) return;
    s_mode = p.getUChar("mode", 1);
    s_delay = p.getUChar("delay", 3);
    char b[96];
    size_t n = p.getBytes("nums", b, sizeof(b) - 1); b[n] = 0; s_nums = b;
    n = p.getBytes("ussd", b, sizeof(b) - 1); b[n] = 0; if (n) s_ussd = b;
    p.end();
}

void smsStart() {
    s_mx = xSemaphoreCreateMutex();
    load();
    xTaskCreatePinnedToCore(task, "sms", 6144, nullptr, 1, &s_task, 0);
}

void smsAlert(const String& text) {
    if (!s_task || !s_mode) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_present && s_nPend < NPEND) s_pend[s_nPend++] = { millis() | 1, text };
    xSemaphoreGive(s_mx);
}

void smsTest() { s_testReq = true; }
void smsBalance() { s_balTo = ""; s_balReq = true; }

String smsLine() {
    if (!s_present) return "";
    String r = s_dbm ? String(s_dbm) + " дБм" : String("нет сигнала");
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_op.length()) r += " · " + s_op;
    xSemaphoreGive(s_mx);
    return r;
}

String smsJson() {
    String nums[NNUM];
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint8_t n = numList(nums);
    String masked;
    for (uint8_t k = 0; k < n; k++) {
        String d = nums[k];
        masked += String(k ? ", " : "") + (d.length() > 6 ? d.substring(0, 4) + "•••" + d.substring(d.length() - 3) : d);
    }
    String j = "{\"on\":true,\"present\":" + String(s_present ? "true" : "false") + ",\"reg\":" + String(s_reg) +
               ",\"dbm\":" + String(s_dbm) + ",\"op\":\"" + jsonEsc(s_op) + "\",\"err\":\"" + jsonEsc(s_err) +
               "\",\"mode\":" + String(s_mode) + ",\"delay\":" + String(s_delay) + ",\"nums\":\"" + jsonEsc(masked) +
               "\",\"n\":" + String(n) + ",\"ussd\":\"" + jsonEsc(s_ussd) + "\",\"bal\":\"" + jsonEsc(s_bal) +
               "\",\"last\":\"" + jsonEsc(s_last) + "\",\"sent\":" + String(s_sent) + ",\"pend\":" + String(s_nPend) + "}";
    xSemaphoreGive(s_mx);
    return j;
}

String smsSet(int mode, const String* nums, int delayMin, const String* ussdCode) {
    if (mode > 2) return "режим — 0, 1 или 2";
    if (delayMin > 60) return "ждать — не больше 60 минут";
    if (nums) {
        String all = *nums, cur;
        uint8_t n = 0;
        for (size_t i = 0; i <= all.length(); i++) {
            char c = i < all.length() ? all[i] : ',';
            if (c == ',' || c == ';') {
                if (cur.length()) {
                    String d = digits(cur);
                    if (d.length() < 10 || d.length() > 15) return "номер «" + cur + "» — нужно 10–15 цифр, например +79781234567";
                    if (++n > NNUM) return "не больше трёх номеров";
                }
                cur = "";
            } else if (c != '-' && c != '(' && c != ')' && c != ' ') cur += c;
        }
    }
    if (ussdCode && (ussdCode->length() < 3 || ussdCode->length() > 20 || (*ussdCode)[0] != '*')) return "код баланса — вида *100#";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (mode >= 0) s_mode = mode;
    if (delayMin >= 0) s_delay = delayMin;
    if (nums) { String t = *nums; t.replace(";", ","); s_nums = t; }
    if (ussdCode) s_ussd = *ussdCode;
    Preferences p;
    if (p.begin("sms", false)) {
        p.putUChar("mode", s_mode);
        p.putUChar("delay", s_delay);
        p.putBytes("nums", s_nums.c_str(), s_nums.length());
        p.putBytes("ussd", s_ussd.c_str(), s_ussd.length());
        p.end();
    }
    xSemaphoreGive(s_mx);
    return "";
}
#endif
